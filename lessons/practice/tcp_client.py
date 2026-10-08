import socket, struct, sys, threading, time

HOST, PORT = "127.0.0.1", int(sys.argv[1])
REQUEST = open(sys.argv[2], "rb").read()

def frame(payload): return struct.pack(">I", len(payload)) + payload

def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk: raise ConnectionError("closed by server")
        data += chunk
    return data

def recv_frame(sock):
    (length,) = struct.unpack(">I", recv_exact(sock, 4))
    return recv_exact(sock, length)

# 1) 8 个连接并发，每个连接一次性"流水线"发出 25 个请求，再依次读回
results, errors = [], []
def worker(index):
    try:
        with socket.create_connection((HOST, PORT)) as sock:
            sock.sendall(frame(REQUEST) * 25)                 # pipelining: 先全部发出
            replies = [recv_frame(sock) for _ in range(25)]   # 按请求顺序收到
            results.append(replies)
    except Exception as exc:
        errors.append(repr(exc))

start = time.time()
threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
for t in threads: t.start()
for t in threads: t.join()
elapsed = time.time() - start
all_replies = [r for replies in results for r in replies]
print(f"concurrent: {len(results)} connections x 25 requests = {len(all_replies)} replies in {elapsed:.2f}s, errors={errors}")
print("every reply starts with {ok, ...}:", all(r[:6] == bytes([131, 104, 2, 119, 2]) + b"o" for r in all_replies))
print("all replies byte-identical (same seed, different threads):", len(set(all_replies)) == 1)

# 2) 同一个连接里：ping、垃圾数据、再 ping —— 垃圾请求只得到 error，连接继续可用
with socket.create_connection((HOST, PORT)) as sock:
    ping = bytes([131, 119, 4]) + b"ping"
    sock.sendall(frame(ping) + frame(bytes([131, 255])) + frame(ping))
    a, b, c = recv_frame(sock), recv_frame(sock), recv_frame(sock)
    print("ping ->", a.hex(" "), "| garbage ->", b[b.find(b"unsupported"):].decode(), "| ping again ->", c == a)

# 3) 声称 4GB 的长度头：服务器应直接断开，而不是去分配内存
with socket.create_connection((HOST, PORT)) as sock:
    sock.sendall(struct.pack(">I", 0xFFFFFFFF))
    try:
        data = sock.recv(10)
        print("oversized frame -> connection closed by server:", data == b"")
    except ConnectionResetError:
        print("oversized frame -> connection reset by server: True")
