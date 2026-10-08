#!/usr/bin/env python3
"""Test client for the gamebattle client protocol (proto/battle_client.proto).

It talks to the test gateway (erlang/src/gamebattle_gateway.erl) over TCP:
every frame is a 4-byte big-endian length plus one protobuf message.

    python battle_client.py play --stage 1
    python battle_client.py load --stage 2 --rate 100 --duration 30

`play` runs one battle and prints the report; `load` sends battles at a fixed
rate and reports throughput, latency and errors. Run with -h for the options.
"""

import argparse
import asyncio
import itertools
import json
import os
import struct
import sys
import time
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_DIR = os.path.normpath(os.path.join(HERE, "..", "proto"))
PROTO_FILE = os.path.join(PROTO_DIR, "battle_client.proto")
GENERATED_DIR = os.path.join(HERE, "generated")

DEFAULT_LINEUPS = {1: "1:1,2:2,3:3,4:4,5:5"}
DETAILS = {"summary": "只要结果", "actions": "按行动聚合的战报", "events": "逐事件的完整战报"}
FULL_LINEUP = "1:1,2:2,3:3,4:4,5:5,6:6,7:7"

EVENT_NAMES = {
    "INITIATIVE": "先手", "ACTION_START": "开始行动", "ACTION_END": "行动结束",
    "SKILL": "发动技能", "PASSIVE": "触发被动", "DAMAGE": "伤害", "DIRECT_DAMAGE": "直接伤害",
    "MISS": "未命中", "HEAL": "治疗", "DEATH": "阵亡", "BUFF_ADD": "添加 Buff",
    "BUFF_REMOVE": "移除 Buff", "BUFF_REACTION": "Buff 反应", "BUFF_EXPIRE": "Buff 结束",
    "CHAIN": "连锁", "NEGATE": "无效", "FIZZLE": "失效",
}
PHASE_NAMES = {"BATTLE": "开场", "ROUND_START": "回合开始", "FIRST_SIDE": "先手方",
               "SECOND_SIDE": "后手方", "ROUND_END": "回合结束"}
SIDE_NAMES = {"ATTACKER": "进攻方", "DEFENDER": "防守方"}
WINNER_NAMES = {"ATTACKER": "进攻方胜", "DEFENDER": "防守方胜", "DRAW": "平局"}
REASON_NAMES = {"INITIAL_STATE": "开场即分胜负", "BATTLE_START": "开场被动分出胜负",
                "ROUND_START": "回合开始时分出胜负", "ALL_UNITS_DEFEATED": "一方全灭",
                "ROUND_END": "回合结束时分出胜负", "MAX_ROUNDS": "达到回合上限",
                "EVENT_LIMIT": "达到事件上限"}


def load_protocol():
    """Generates battle_client_pb2 from the .proto when missing or outdated."""
    target = os.path.join(GENERATED_DIR, "battle_client_pb2.py")
    if not os.path.exists(target) or os.path.getmtime(target) < os.path.getmtime(PROTO_FILE):
        try:
            from grpc_tools import protoc
        except ImportError:
            sys.exit("需要先安装依赖：pip install -r requirements.txt")
        os.makedirs(GENERATED_DIR, exist_ok=True)
        if protoc.main(["protoc", f"-I{PROTO_DIR}", f"--python_out={GENERATED_DIR}", PROTO_FILE]) != 0:
            sys.exit("生成 protobuf 代码失败")
    sys.path.insert(0, GENERATED_DIR)
    import battle_client_pb2
    return battle_client_pb2


def enum_label(pb, enum_name, value, names):
    """'EVENT_TYPE_DAMAGE' -> '伤害'; unknown values (a newer server) stay readable."""
    enum = getattr(pb, enum_name)
    try:
        symbol = enum.Name(value)
    except ValueError:
        return f"未知({value})"
    prefix = "".join("_" + c if c.isupper() else c for c in enum_name).lstrip("_").upper() + "_"
    short = symbol[len(prefix):] if symbol.startswith(prefix) else symbol
    return names.get(short, short)


def pad(text, width):
    """Left-aligns text to a terminal width; CJK characters take two columns."""
    used = sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in text)
    return text + " " * max(0, width - used)


def parse_lineup(text):
    lineup = []
    for item in text.split(","):
        unit, _, position = item.partition(":")
        lineup.append((int(unit), int(position or 0)))
    return lineup


def start_battle(pb, request_id, stage, lineup, detail):
    message = pb.ClientMessage(request_id=request_id)
    message.start_battle.stage_id = stage
    message.start_battle.detail = pb.ReportDetail.Value("REPORT_DETAIL_" + detail.upper())
    for unit, position in lineup:
        message.start_battle.lineup.add(unit_id=unit, position=position)
    body = message.SerializeToString()
    return struct.pack(">I", len(body)) + body


async def read_frame(reader):
    (length,) = struct.unpack(">I", await reader.readexactly(4))
    return await reader.readexactly(length)


def varint(data, offset):
    result = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if byte < 0x80:
            return result, offset
        shift += 7


def envelope(data):
    """request_id and body field number of a ServerMessage, without decoding the
    (possibly megabyte-sized) report: 10 error, 11 battle report, 12 gauntlet."""
    offset, request_id, body = 0, 0, None
    while offset < len(data):
        key, offset = varint(data, offset)
        field, wire_type = key >> 3, key & 7
        if wire_type == 0:
            value, offset = varint(data, offset)
            if field == 1:
                request_id = value
        elif wire_type == 2:
            length, offset = varint(data, offset)
            if field in (10, 11, 12):
                body = field
            offset += length
        elif wire_type == 1:
            offset += 8
        elif wire_type == 5:
            offset += 4
        else:
            raise ValueError(f"unexpected wire type {wire_type}")
    return request_id, body


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(len(sorted_values) * p / 100))]


# ---------------------------------------------------------------- play

async def play(args, pb):
    lineup = parse_lineup(args.lineup or DEFAULT_LINEUPS.get(args.stage, FULL_LINEUP))
    reader, writer = await asyncio.open_connection(args.host, args.port)
    started = time.perf_counter()
    writer.write(start_battle(pb, 1, args.stage, lineup, args.detail))
    await writer.drain()
    data = await read_frame(reader)
    elapsed = (time.perf_counter() - started) * 1000
    writer.close()

    reply = pb.ServerMessage.FromString(data)
    if reply.WhichOneof("body") == "error":
        code = enum_label(pb, "ErrorCode", reply.error.code, {})
        sys.exit(f"服务端返回错误 {code}：{reply.error.message}")
    report = reply.battle_report

    print(f"战斗 {report.battle_id}（种子 {report.seed}）")
    print(f"结果：{enum_label(pb, 'Winner', report.winner, WINNER_NAMES)}，"
          f"{enum_label(pb, 'EndReason', report.reason, REASON_NAMES)}，{report.rounds} 回合，"
          f"先手值 {report.attacker_initiative} : {report.defender_initiative}")
    if report.actions:
        size = f"{len(report.actions):,} 步，代表 {sum(a.event_count for a in report.actions):,} 个事件"
    elif report.events:
        size = f"{len(report.events):,} 个事件"
    else:
        size = "只有结果"
    print(f"往返 {elapsed:.1f} ms，战报 {len(data):,} 字节，{size}\n")

    print(f"{pad('单位', 8)}{pad('阵营', 8)}{pad('初始HP', 14)}{pad('最终HP', 14)}{pad('最大HP', 14)}存活")
    for unit in report.units:
        side = enum_label(pb, "Side", unit.side, SIDE_NAMES)
        print(f"{pad(str(unit.id), 8)}{pad(side, 8)}{pad(f'{unit.initial_hp:,}', 14)}"
              f"{pad(f'{unit.hp:,}', 14)}{pad(f'{unit.max_hp:,}', 14)}{'是' if unit.alive else '否'}")

    if report.actions:
        shown = report.actions if args.show == 0 else report.actions[:args.show]
        print(f"\n战斗过程（显示 {len(shown):,} / {len(report.actions):,} 步，--show 0 显示全部）")
        for index, action in enumerate(shown, 1):
            print(format_action(pb, index, action))
        actions = sum(1 for a in report.actions if a.actor)
        print(f"\n共 {len(report.actions):,} 步：单位行动 {actions:,}，"
              f"行动之外的触发 {len(report.actions) - actions:,}")

    if report.events:
        shown = report.events if args.show == 0 else report.events[:args.show]
        print(f"\n事件（显示 {len(shown):,} / {len(report.events):,} 条，--show 0 显示全部）")
        for event in shown:
            print(format_event(pb, event))
        counts = {}
        for event in report.events:
            label = enum_label(pb, "EventType", event.type, EVENT_NAMES)
            counts[label] = counts.get(label, 0) + 1
        print("\n事件统计：" + "，".join(f"{k} {v:,}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])))

    if args.json:
        from google.protobuf.json_format import MessageToDict
        with open(args.json, "w", encoding="utf-8") as out:
            json.dump(MessageToDict(report, preserving_proto_field_name=True), out, ensure_ascii=False, indent=1)
        print(f"\n完整战报已写入 {args.json}")


def format_action(pb, index, action):
    phase = enum_label(pb, "Phase", action.phase, PHASE_NAMES)
    text = f"{index:>6}  {pad(f'第{action.round}回合', 10)}{pad(phase, 10)}"
    if action.actor:
        side = enum_label(pb, "Side", action.side, SIDE_NAMES)
        skill = f"技能 {action.skill_id}" if action.skill_id else "普攻"
        text += f"{action.actor}（{side}）{skill}"
    else:
        text += "触发"
    for unit in action.units:
        parts = []
        if unit.hits:
            parts.append(f"受到 {unit.damage:,}（{unit.hits} 击" + (f"，{unit.crits} 暴击" if unit.crits else "") + "）")
        if unit.misses:
            parts.append(f"闪避 {unit.misses}")
        if unit.heal:
            parts.append(f"治疗 {unit.heal:,}")
        if not unit.died or parts:  # a heal of 0 (already at full HP) leaves only the HP
            parts.append(f"HP {unit.hp:,}")
        if unit.died:
            parts.append("阵亡")
        text += f"\n          → {unit.unit} " + "，".join(parts)
    if action.effects:
        effects = [enum_label(pb, "EventType", e.type, EVENT_NAMES)
                   + (f" {e.source_id}" if e.source_id else "") + (f"@{e.unit}" if e.unit else "")
                   + (f" ×{e.count}" if e.count > 1 else "") for e in action.effects]
        more = f" 等 {len(effects)} 项" if len(effects) > 6 else ""
        text += "\n          效果：" + "，".join(effects[:6]) + more
    return text + f"  [{action.event_count:,} 个事件]"


def format_event(pb, event):
    kind = enum_label(pb, "EventType", event.type, EVENT_NAMES)
    phase = enum_label(pb, "Phase", event.phase, PHASE_NAMES)
    text = f"{event.seq:>7}  {pad(f'第{event.round}回合', 10)}{pad(phase, 10)}{pad(kind, 10)}{event.actor}"
    if event.target:
        text += f" → {event.target}"
    if event.source_id:
        text += f"  来源 {event.source_id}"
    if event.value:
        text += f"  数值 {event.value:,}"
    if event.hp_before or event.hp_after:
        text += f"  HP {event.hp_before:,}→{event.hp_after:,}"
    if event.critical:
        text += "  暴击"
    return text


# ---------------------------------------------------------------- load

class LoadStats:
    def __init__(self):
        self.sent = self.ok = self.busy = self.errors = self.bytes = 0
        self.latencies = []
        self.window = []
        self.error_codes = {}


async def receive(reader, pending, stats, pb):
    while True:
        try:
            data = await read_frame(reader)
        except (asyncio.IncompleteReadError, ConnectionError):
            return
        now = time.perf_counter()
        request_id, body = envelope(data)
        sent_at = pending.pop(request_id, None)
        stats.bytes += len(data) + 4
        if body == 11:
            stats.ok += 1
            if sent_at is not None:
                latency = (now - sent_at) * 1000
                stats.latencies.append(latency)
                stats.window.append(latency)
        else:
            code = enum_label(pb, "ErrorCode", pb.ServerMessage.FromString(data).error.code, {})
            if code == "RETRY_LATER":
                stats.busy += 1
            else:
                stats.errors += 1
                stats.error_codes[code] = stats.error_codes.get(code, 0) + 1


async def load(args, pb):
    lineup = parse_lineup(args.lineup or DEFAULT_LINEUPS.get(args.stage, FULL_LINEUP))
    connections = [await asyncio.open_connection(args.host, args.port) for _ in range(args.connections)]
    pending, stats = {}, LoadStats()
    receivers = [asyncio.create_task(receive(r, pending, stats, pb)) for r, _ in connections]
    request_ids = itertools.count(1)

    print(f"目标 {args.rate} 场/秒，持续 {args.duration} 秒，{args.connections} 个连接，关卡 {args.stage}，"
          f"{DETAILS[args.detail]}")
    print(f"{'秒':>4} {'发出':>6} {'完成':>6} {'繁忙':>6} {'错误':>5} {'在途':>6} "
          f"{'本秒p50':>9} {'本秒p99':>9} {'接收MB/s':>9}")
    interval = 1.0 / args.rate
    start = time.perf_counter()
    next_report, last_bytes, last_ok = start + 1, 0, 0
    for n in itertools.count():
        due = start + n * interval
        if due - start >= args.duration:
            break
        delay = due - time.perf_counter()
        if delay > 0:
            await asyncio.sleep(delay)
        request_id = next(request_ids) & 0xFFFFFFFF
        _, writer = connections[n % len(connections)]
        pending[request_id] = time.perf_counter()
        writer.write(start_battle(pb, request_id, args.stage, lineup, args.detail))
        stats.sent += 1
        if writer.transport.get_write_buffer_size() > 1 << 20:
            await writer.drain()
        if time.perf_counter() >= next_report:
            window = sorted(stats.window)
            stats.window = []
            print(f"{int(next_report - start):>4} {stats.sent:>6} {stats.ok:>6} {stats.busy:>6} {stats.errors:>5} "
                  f"{len(pending):>6} {percentile(window, 50):>8.0f}ms {percentile(window, 99):>8.0f}ms "
                  f"{(stats.bytes - last_bytes) / 1048576:>9.2f}")
            last_bytes, last_ok = stats.bytes, stats.ok
            next_report += 1

    sending_time = time.perf_counter() - start
    deadline = time.perf_counter() + args.drain
    while pending and time.perf_counter() < deadline:
        await asyncio.sleep(0.05)
    total_time = time.perf_counter() - start
    for task in receivers:
        task.cancel()
    for _, writer in connections:
        writer.close()

    latencies = sorted(stats.latencies)
    finished = stats.ok
    print(f"\n== 结果")
    print(f"发出 {stats.sent:,} 场（{stats.sent / sending_time:.1f} 场/秒），完成 {finished:,} 场"
          f"（{finished / total_time:.1f} 场/秒），繁忙拒绝 {stats.busy:,}，其他错误 {stats.errors:,}"
          f"{' ' + str(stats.error_codes) if stats.error_codes else ''}，超时未回 {len(pending):,}")
    if latencies:
        print(f"完成的战斗耗时：p50 {percentile(latencies, 50):.0f} ms，p90 {percentile(latencies, 90):.0f} ms，"
              f"p99 {percentile(latencies, 99):.0f} ms，最大 {latencies[-1]:.0f} ms")
    if stats.ok:
        print(f"接收 {stats.bytes / 1048576:,.1f} MB（{stats.bytes / 1048576 / total_time:,.2f} MB/s，"
              f"每场平均 {stats.bytes / (stats.ok + stats.busy + stats.errors):,.0f} 字节）")
    held = stats.busy == 0 and stats.errors == 0 and not pending and finished >= 0.95 * stats.sent
    if held:
        print(f"结论：扛住了 {args.rate} 场/秒，p99 {percentile(latencies, 99):.0f} ms。")
    else:
        print(f"结论：没扛住 {args.rate} 场/秒，服务端实际处理能力约 {finished / total_time:.1f} 场/秒。"
              f"增加战斗核数（GAMEBATTLE_BATTLE_WORKERS）或换更快的引擎后再试。")


def main():
    parser = argparse.ArgumentParser(description="gamebattle 协议测试客户端")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7000)
    commands = parser.add_subparsers(dest="command", required=True)

    play_parser = commands.add_parser("play", help="打一场战斗并打印战报")
    play_parser.add_argument("--stage", type=int, default=1, help="关卡：1 普通，2 混合被动压力，3 连锁压力")
    play_parser.add_argument("--lineup", help="阵容 英雄:位置,...，英雄 1-7，例如 1:1,2:2,3:3")
    play_parser.add_argument("--detail", choices=DETAILS, default="actions",
                             help="战报详细程度：summary 只要结果，actions 按行动聚合（默认），events 逐事件")
    play_parser.add_argument("--show", "--events", type=int, default=40, help="打印前几步或前几条事件，0 表示全部")
    play_parser.add_argument("--json", help="把完整战报写成 JSON 文件")

    load_parser = commands.add_parser("load", help="按固定速率发起战斗，测吞吐和耗时")
    load_parser.add_argument("--stage", type=int, default=1)
    load_parser.add_argument("--lineup")
    load_parser.add_argument("--rate", type=float, default=100, help="每秒发起多少场")
    load_parser.add_argument("--duration", type=float, default=30, help="持续多少秒")
    load_parser.add_argument("--connections", type=int, default=8, help="用多少个 TCP 连接")
    load_parser.add_argument("--detail", choices=DETAILS, default="actions",
                             help="战报详细程度，默认 actions；summary 排除战报的开销")
    load_parser.add_argument("--drain", type=float, default=60, help="发完后最多再等多少秒收回复")

    args = parser.parse_args()
    pb = load_protocol()
    asyncio.run(play(args, pb) if args.command == "play" else load(args, pb))


if __name__ == "__main__":
    main()
