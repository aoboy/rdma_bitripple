# BitRipple Library Makefile — RDMA/RoCE Edition
# Builds libnanorq + libbitripple (static & shared)
# nanorq is cloned with --recursive-submodules and built in-tree.

CC      = gcc
AR      = ar
CFLAGS  = -Wall -Wextra -O3 -g -std=c99 -fPIC
CPPFLAGS = -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64

# RDMA libraries
RDMA_LIBS = -libverbs -lrdmacm

# ---------------------------------------------------------------------
# nanorq paths
# ---------------------------------------------------------------------
NANORQ_DIR      = nanorq
NANORQ_INC      = $(NANORQ_DIR)/include
OBLAS_DIR       = $(NANORQ_DIR)/deps/oblas
OBLAS_INC       = $(OBLAS_DIR)

NANORQ_SRCS     = $(NANORQ_DIR)/lib/bitmask.c \
                  $(NANORQ_DIR)/lib/io.c \
                  $(NANORQ_DIR)/lib/nanorq.c \
                  $(NANORQ_DIR)/lib/params.c \
                  $(NANORQ_DIR)/lib/precode.c \
                  $(NANORQ_DIR)/lib/rand.c \
                  $(NANORQ_DIR)/lib/sched.c \
                  $(NANORQ_DIR)/lib/spmat.c \
                  $(NANORQ_DIR)/lib/tuple.c \
                  $(NANORQ_DIR)/lib/wrkmat.c

NANORQ_OBJS     = $(NANORQ_SRCS:.c=.o)
OBLAS_LIB       = $(OBLAS_DIR)/liboblas.a

# ---------------------------------------------------------------------
# bitripple sources (RDMA edition)
# ---------------------------------------------------------------------
BITRIPPLE_SRCS  = src/bitripple.c \
                  src/bitripple_profile.c \
                  src/bitripple_spray.c \
                  src/bitripple_codec.c \
                  src/bitripple_net.c \
                  src/bitripple_client.c \
                  src/bitripple_server.c \
                  src/bitripple_feedback.c \
                  src/bitripple_rdma.c

BITRIPPLE_OBJS  = $(BITRIPPLE_SRCS:.c=.o)
BITRIPPLE_INCS  = -I./include -I$(NANORQ_INC) -I$(OBLAS_INC)

.PHONY: all clean test clone-nanorq

all: libnanorq.a libnanorq.so libbitripple.a libbitripple.so

# ---------------------------------------------------------------------
# Clone nanorq with recursive submodules if missing
# ---------------------------------------------------------------------
clone-nanorq:
	@if [ ! -f "$(NANORQ_DIR)/Makefile" ]; then \
		echo "Cloning nanorq with submodules..."; \
		git clone --recurse-submodules https://github.com/sleepybishop/nanorq.git $(NANORQ_DIR); \
	fi

# ---------------------------------------------------------------------
# Build oblas dependency (submodule inside nanorq)
# ---------------------------------------------------------------------
$(OBLAS_LIB): | clone-nanorq
	$(MAKE) -C $(OBLAS_DIR) CPPFLAGS+="-DOBLAS_AVX -DOCTMAT_ALIGN=32"

# ---------------------------------------------------------------------
# nanorq object compilation
# ---------------------------------------------------------------------
$(NANORQ_DIR)/lib/%.o: $(NANORQ_DIR)/lib/%.c | clone-nanorq $(OBLAS_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-I$(NANORQ_DIR) -I$(NANORQ_INC) -I$(OBLAS_INC) \
		-c $< -o $@

# ---------------------------------------------------------------------
# libnanorq.a (static) — bundles oblas objects
# ---------------------------------------------------------------------
libnanorq.a: $(NANORQ_OBJS) $(OBLAS_LIB)
	$(AR) rcs $@ $(NANORQ_OBJS) $(OBLAS_DIR)/*.o

# ---------------------------------------------------------------------
# libnanorq.so (shared)
# ---------------------------------------------------------------------
libnanorq.so: $(NANORQ_SRCS) $(OBLAS_LIB) | clone-nanorq
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC \
		-I$(NANORQ_DIR) -I$(NANORQ_INC) -I$(OBLAS_INC) \
		$(NANORQ_SRCS) $(OBLAS_DIR)/*.o -o $@

# ---------------------------------------------------------------------
# libbitripple
# ---------------------------------------------------------------------
$(BITRIPPLE_OBJS): %.o: %.c include/bitripple.h include/bitripple_types.h include/bitripple_rdma_internal.h | clone-nanorq
	$(CC) $(CFLAGS) $(BITRIPPLE_INCS) -c $< -o $@

libbitripple.a: $(BITRIPPLE_OBJS)
	$(AR) rcs $@ $^

libbitripple.so: $(BITRIPPLE_OBJS) libnanorq.a
	$(CC) -shared -o $@ $(BITRIPPLE_OBJS) -L. -lnanorq $(RDMA_LIBS) -lm

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------
TEST_IP = 172.28.49.50
TEST_PORT = 13337
TEST_PORT_FB = 13338

test: all
	LD_LIBRARY_PATH=. python3 tests/test_library.py
	LD_LIBRARY_PATH=. python3 tests/test_library_net.py

test_rdma_transport: libbitripple.so libnanorq.so
	$(CC) $(CFLAGS) tests/test_rdma_transport.c -o $@ \
		-I./include -I$(NANORQ_INC) -I$(OBLAS_INC) \
		-L. -lbitripple -lnanorq $(RDMA_LIBS) -lm -lpthread \
		-Wl,-rpath,.

# Run server in background and client for combined test
# Uses timeout to kill hanging processes
test_rdma_transport_combined: test_rdma_transport
	@echo "=== Stopping any existing processes on ports $(TEST_PORT) and $(TEST_PORT_FB) ===" && \
	fuser -k $(TEST_PORT)/tcp 2>/dev/null || true && \
	fuser -k $(TEST_PORT_FB)/tcp 2>/dev/null || true && \
	sleep 1 && \
	echo "=== Starting transport test server ===" && \
	LD_LIBRARY_PATH=. ./test_rdma_transport -m server -p $(TEST_PORT) -n 2 > transport_server.log 2>&1 & \
	SERVER_PID=$$! && \
	echo "Server PID: $$SERVER_PID" && \
	sleep 3 && \
	echo "=== Starting transport test client (small data) ===" && \
	LD_LIBRARY_PATH=. timeout 30 ./test_rdma_transport -m client -a $(TEST_IP) -p $(TEST_PORT) -n 2 -s small 2>&1 || CLIENT_RC=$$?; \
	echo "=== Stopping server (PID $$SERVER_PID) ===" && \
	kill -TERM $$SERVER_PID 2>/dev/null || true && \
	sleep 1 && \
	kill -9 $$SERVER_PID 2>/dev/null || true && \
	sleep 1 && \
	echo "=== Server log ===" && \
	cat transport_server.log && \
	echo "" && \
	echo "=== Transport test complete ==="

test_rdma_transport_combined_large: test_rdma_transport
	@echo "=== Stopping any existing processes on ports $(TEST_PORT) and $(TEST_PORT_FB) ===" && \
	fuser -k $(TEST_PORT)/tcp 2>/dev/null || true && \
	fuser -k $(TEST_PORT_FB)/tcp 2>/dev/null || true && \
	sleep 1 && \
	echo "=== Starting transport test server (large) ===" && \
	LD_LIBRARY_PATH=. ./test_rdma_transport -m server -p $(TEST_PORT) -n 2 > transport_server_large.log 2>&1 & \
	SERVER_PID=$$! && \
	echo "Server PID: $$SERVER_PID" && \
	sleep 3 && \
	echo "=== Starting transport test client (large data) ===" && \
	LD_LIBRARY_PATH=. timeout 60 ./test_rdma_transport -m client -a $(TEST_IP) -p $(TEST_PORT) -n 2 -s large 2>&1 || CLIENT_RC=$$?; \
	echo "=== Stopping server (PID $$SERVER_PID) ===" && \
	kill -TERM $$SERVER_PID 2>/dev/null || true && \
	sleep 1 && \
	kill -9 $$SERVER_PID 2>/dev/null || true && \
	sleep 1 && \
	echo "=== Server log ===" && \
	cat transport_server_large.log && \
	echo "" && \
	echo "=== Transport test (large) complete ==="

# Custom size test
test_rdma_transport_custom: test_rdma_transport
	@if [ -z "$(SIZE)" ]; then \
		echo "Usage: make test_rdma_transport_custom SIZE=<bytes>"; \
		echo "Examples:"; \
		echo "  make test_rdma_transport_custom SIZE=1024"; \
		echo "  make test_rdma_transport_custom SIZE=1048576"; \
		exit 1; \
	fi
	@echo "=== Stopping any existing processes on ports $(TEST_PORT) and $(TEST_PORT_FB) ===" && \
	fuser -k $(TEST_PORT)/tcp 2>/dev/null || true && \
	fuser -k $(TEST_PORT_FB)/tcp 2>/dev/null || true && \
	sleep 1 && \
	echo "=== Starting transport test server ($(SIZE) bytes) ===" && \
	LD_LIBRARY_PATH=. ./test_rdma_transport -m server -p $(TEST_PORT) -n 2 > transport_server_custom.log 2>&1 & \
	SERVER_PID=$$! && \
	echo "Server PID: $$SERVER_PID" && \
	sleep 3 && \
	echo "=== Starting transport test client ($(SIZE) bytes) ===" && \
	LD_LIBRARY_PATH=. timeout 60 ./test_rdma_transport -m client -a $(TEST_IP) -p $(TEST_PORT) -n 2 -s $(SIZE) 2>&1 || CLIENT_RC=$$?; \
	echo "=== Stopping server (PID $$SERVER_PID) ===" && \
	kill -TERM $$SERVER_PID 2>/dev/null || true && \
	sleep 1 && \
	kill -9 $$SERVER_PID 2>/dev/null || true && \
	sleep 1 && \
	echo "=== Server log ===" && \
	cat transport_server_custom.log && \
	echo "" && \
	echo "=== Transport test ($(SIZE) bytes) complete ==="

clean:
	rm -f src/*.o $(NANORQ_DIR)/lib/*.o lib*.a lib*.so
	rm -f test_rdma_transport transport_server.log transport_server_large.log transport_server_custom.log
	if [ -f "$(OBLAS_DIR)/Makefile" ]; then \
		$(MAKE) -C $(OBLAS_DIR) clean; \
	fi
