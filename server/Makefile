# ClockReader 书房服务端
#
# 目标机：局域网 Linux（Intel Atom，i386）。
# 本机没有 Go 也没关系——交叉编译出单文件二进制拷过去即可。

BIN      := clockreader-server-linux-386
GO       ?= go
CGO      := 0
GOOS     := linux
GOARCH   := 386
LDFLAGS  := -s -w

.PHONY: all build cross run clean

all: cross

## 本机目标（开发时快速查错）
build:
	$(GO) build ./...

## i386 交叉编译：纯 Go（modernc.org/sqlite 不需要 CGO），
## 所以不依赖 i686 的 gcc，任何装了 Go 的机器都能出包。
cross:
	CGO_ENABLED=$(CGO) GOOS=$(GOOS) GOARCH=$(GOARCH) \
		$(GO) build -trimpath -ldflags="$(LDFLAGS)" -o $(BIN) .

run: build
	$(GO) run .

clean:
	rm -f $(BIN)
