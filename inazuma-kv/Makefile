CC = gcc
CXX = g++
CLANG = clang

# ==========================================
# 目录配置 (严禁在行尾加注释和空格)
# ==========================================
INC_DIR = include
SRC_DIR = src
TEST_DIR = tests
TOOL_DIR = tools
EBPF_DIR = ebpf
CONF_DIR = conf
LIB_DIR = libs/NtyCo
BIN_DIR = bin
OBJ_DIR = obj

# ==========================================
# 网络模型选择 (仅对 kvstore 业务进程生效)
# ==========================================
NETWORK_MODEL = REACTOR

# ==========================================
# 编译参数与库 (注入终极性能优化核武器)
# ==========================================
ENABLE_ASAN = 0

# 🎯 核心修改：加入 C++ 和 FAISS 向量引擎需要的四大底层库
LIBS = -lpthread -ldl -ljemalloc -lhiredis -lbpf -luring -lxdp -lelf -lz -lrdmacm -libverbs -lstdc++ -lfaiss -fopenmp -lopenblas

# 🎯 核心修改：加入 -rdynamic 暴露符号表(供黑匣子抓取函数名)
CFLAGS_BASE = -I./$(INC_DIR) -I./$(LIB_DIR)/core/ -DENABLE_JEMALLOC=1 -g -rdynamic -O3 -march=native -funroll-loops -Wall -fno-omit-frame-pointer -MMD -MP

ifeq ($(ENABLE_ASAN), 1)
    LIBS += -fsanitize=address
    CFLAGS_BASE += -fsanitize=address
endif

# ==========================================
# 源文件精细化分组 (企业级解耦)
# ==========================================
# 1. 核心共享引擎 (业务逻辑、存储结构、配置、持久化、复制与RDMA、遥测探针、灾难黑匣子、无锁共享内存)
CORE_SRCS = $(SRC_DIR)/kvstore.c $(SRC_DIR)/kvs_array.c $(SRC_DIR)/kvs_rbtree.c \
    $(SRC_DIR)/kvs_hash.c $(SRC_DIR)/kvs_skiplist.c $(SRC_DIR)/kvs_persist.c \
    $(SRC_DIR)/kvs_replication.c $(SRC_DIR)/kvs_rdma.c $(SRC_DIR)/kvs_config.c \
    $(SRC_DIR)/mem_probe.c $(SRC_DIR)/kvs_crash.c $(SRC_DIR)/shm_ipc.c

# 🎯 新增: C++ 向量引擎桥接文件
CORE_CXX_SRCS = $(SRC_DIR)/kvs_vector.cpp

# 2. KV 数据库专属代码 (入口与 TCP 网络模型)
KV_MAIN_SRC = $(SRC_DIR)/main.c

# 3. 独立 XDP 网关专属代码 (入口与网卡接管)
GW_SRCS = $(SRC_DIR)/xdp_gateway.c $(SRC_DIR)/xdp_module.c

# 4. eBPF 内核态程序
BPF_KERN_SRC = $(EBPF_DIR)/xdp_prog.c
BPF_OBJ = $(BIN_DIR)/xdp_prog.o

# ==========================================
# 独立二进制目标
# ==========================================
TARGET_KV = $(BIN_DIR)/kvstore
TARGET_GW = $(BIN_DIR)/xdp_gateway

# ==========================================
# 模型分支处理 (拼装 KV 专属的编译组件)
# ==========================================
ifeq ($(NETWORK_MODEL), NTYCO)
    KV_NET_SRC = $(SRC_DIR)/ntyco.c
    FLAGS = $(CFLAGS_BASE) $(LIBS) -L./$(LIB_DIR)/ -lntyco -DNETWORK_SELECT=NETWORK_NTYCO
    PREREQUISITES = $(LIB_DIR) dirs
else ifeq ($(NETWORK_MODEL), REACTOR)
    KV_NET_SRC = $(SRC_DIR)/reactor.c
    FLAGS = $(CFLAGS_BASE) $(LIBS) -DNETWORK_SELECT=NETWORK_REACTOR
    PREREQUISITES = dirs
else ifeq ($(NETWORK_MODEL), PROACTOR)
    KV_NET_SRC = $(SRC_DIR)/proactor.c
    FLAGS = $(CFLAGS_BASE) $(LIBS) -DNETWORK_SELECT=NETWORK_PROACTOR
    PREREQUISITES = dirs
endif

# 将所有源文件映射为 .o
CORE_OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(CORE_SRCS))
CORE_CXX_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(CORE_CXX_SRCS))
KV_MAIN_OBJ = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(KV_MAIN_SRC))
KV_NET_OBJ = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(KV_NET_SRC))
GW_OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(GW_SRCS))

ALL_OBJS = $(CORE_OBJS) $(CORE_CXX_OBJS) $(KV_MAIN_OBJ) $(KV_NET_OBJ) $(GW_OBJS)
DEPS = $(ALL_OBJS:.o=.d)

# ==========================================
# 编译规则 (多目标输出)
# ==========================================
all: $(PREREQUISITES) $(BPF_OBJ) $(TARGET_KV) $(TARGET_GW) tests tools

dirs:
	@mkdir -p $(BIN_DIR) $(OBJ_DIR)

# eBPF 字节码编译
$(BPF_OBJ): $(BPF_KERN_SRC) | dirs
	$(CLANG) -O2 -g -target bpf -I./$(INC_DIR) -c $< -o $@

# 目标 1：生成纯粹的 KV 数据库 (🎯 修改：这里改为 $(CXX))
$(TARGET_KV): $(CORE_OBJS) $(CORE_CXX_OBJS) $(KV_NET_OBJ) $(KV_MAIN_OBJ)
	$(CXX) -o $@ $^ $(FLAGS)

# 目标 2：生成独立的 XDP 数据面网关 (🎯 修改：这里改为 $(CXX))
$(TARGET_GW): $(GW_OBJS) $(OBJ_DIR)/kvs_config.o $(OBJ_DIR)/shm_ipc.o
	$(CXX) -o $@ $^ $(FLAGS)

$(LIB_DIR): ECHO
	make -C $@

ECHO:
	@echo "Building Library in $(LIB_DIR)..."

# 通用 .c 编译规则
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	$(CC) $(FLAGS) -c $< -o $@

# 🎯 新增: 通用 .cpp 编译规则 (使用 g++ 编译 FAISS 桥接层)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | dirs
	$(CXX) $(FLAGS) -c $< -o $@

# ==========================================
# 测试用例与工具 (防污染)
# ==========================================
TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TOOL_SRCS = $(wildcard $(TOOL_DIR)/*.c)

TEST_BINS = $(patsubst $(TEST_DIR)/%.c, $(BIN_DIR)/%, $(TEST_SRCS))
TOOL_BINS = $(patsubst $(TOOL_DIR)/%.c, $(BIN_DIR)/%, $(TOOL_SRCS))

tests: $(TEST_BINS)
tools: $(TOOL_BINS)

# 测试用例与工具 (🎯 修改：这里改为 $(CXX))
$(TEST_BINS): $(BIN_DIR)/%: $(TEST_DIR)/%.c $(CORE_OBJS) $(CORE_CXX_OBJS) $(KV_NET_OBJ) | dirs
	$(CXX) -o $@ $< $(CORE_OBJS) $(CORE_CXX_OBJS) $(KV_NET_OBJ) $(FLAGS)

$(TOOL_BINS): $(BIN_DIR)/%: $(TOOL_DIR)/%.c $(CORE_OBJS) $(CORE_CXX_OBJS) $(KV_NET_OBJ) | dirs
	$(CXX) -o $@ $< $(CORE_OBJS) $(CORE_CXX_OBJS) $(KV_NET_OBJ) $(FLAGS)

# ==========================================
# 清理
# ==========================================
clean:
	rm -rf $(BIN_DIR) $(OBJ_DIR)
	@echo "[System] Build artifacts cleaned."

clean-data:
	rm -f data/kvs.rdb data/kvs.aof kvs.rdb kvs.aof
	@echo "[System] Persistence data (RDB/AOF) cleared."

reset: clean clean-data
	@echo "[System] Factory reset complete."

-include $(DEPS)