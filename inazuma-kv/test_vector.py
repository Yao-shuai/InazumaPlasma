import redis
import struct
import random
import time

# ==========================================
# ⚙️ 配置区
# ==========================================
DIMENSION = 1536  # 必须和你在 kvstore.c 里 C++ 引擎初始化的维度 100% 保持一致！
HOST = '127.0.0.1'
PORT = 6379

def floats_to_binary(vec):
    """
    ⚡ 核心魔法：将 Python 的浮点数列表，直接内存级压扁成 C 语言认识的纯二进制字节流！
    'f' * DIMENSION 表示连续打包 1536 个单精度 float (32-bit)
    """
    return struct.pack(f'{DIMENSION}f', *vec)

def generate_random_vector():
    return [random.random() for _ in range(DIMENSION)]

def main():
    print(f"🔌 正在连接 InazumaKV ({HOST}:{PORT})...")
    r = redis.Redis(host=HOST, port=PORT)

    # 1. 制造三个假数据 (模拟大模型生成的向量)
    print("🧬 正在生成 1536 维的高维向量...")
    vec_apple = generate_random_vector()
    vec_banana = generate_random_vector()
    
    # 我们故意造一个和 Apple 极度相似的向量 (只改动一丁点)
    vec_query = vec_apple.copy()
    vec_query[0] += 0.001 

    # 2. VADD：将向量写入底座
    print("🚀 [VADD] 开始将二进制向量零拷贝打入底座...")
    start_t = time.time()
    
    # 命令格式：VADD <key> <binary_floats>
    r.execute_command('VADD', 'doc:apple_description', floats_to_binary(vec_apple))
    r.execute_command('VADD', 'doc:banana_description', floats_to_binary(vec_banana))
    
    print(f"✅ 写入完成！耗时: {(time.time() - start_t)*1000:.2f} ms")

    # 3. VSEARCH：执行高维相似度检索
    print("\n🔍 [VSEARCH] 拿着 Query 向量去庞大的宇宙中寻找最相似的 Top-2...")
    start_t = time.time()
    
    # 命令格式：VSEARCH <binary_floats> <Top-K>
    results = r.execute_command('VSEARCH', floats_to_binary(vec_query), 2)
    
    print(f"⏱️ 检索耗时: {(time.time() - start_t)*1000:.2f} ms\n")
    
    # 4. 解析战果
    print("🎯 === 检索结果 (InazumaKV 原生返回) ===")
    # Redis-py 返回的是 bytes 列表：[b'key1', b'dist1', b'key2', b'dist2']
    for i in range(0, len(results), 2):
        key = results[i].decode('utf-8')
        distance = results[i+1].decode('utf-8')
        rank = (i // 2) + 1
        print(f"🏆 排名 {rank} | 匹配 Key: {key} | L2 距离: {distance}")

if __name__ == '__main__':
    main()