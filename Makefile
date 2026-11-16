# wxWidgets および MinGW g++ による静的リンクビルド用 Makefile

CXX = g++
CXXFLAGS = -O2 -std=c++17 -D_UNICODE -DUNICODE -D__WXMSW__ -mthreads -Wall -fmacro-prefix-map=$(WX_DIR)=wxWidgets -fdebug-prefix-map=$(WX_DIR)=wxWidgets

# wxWidgets インストールディレクトリ (環境変数または引数 WX_DIR で指定)
WX_DIR ?= C:/path/to/wxWidgets-3.2.6
WX_INC = -I$(WX_DIR)/include -I$(WX_DIR)/lib/gcc_lib/mswu
WX_LIB_DIR = -L$(WX_DIR)/lib/gcc_lib

# wxWidgets静的ライブラリ (core, base, adv, net, PNG画像コーデック等)
# MIT互換かつ軽量化のため TIFF/JPEG は除外し、PNG (+標準BMP) のみリンクする
WX_LIBS = -lwxmsw32u_adv -lwxmsw32u_core -lwxbase32u_net -lwxbase32u \
          -lwxpng -lwxzlib -lwxregexu -lwxexpat

# Windows標準ライブラリ
WIN_LIBS = -lwininet -lgdiplus -lcomctl32 -lole32 -luuid -lshlwapi -lversion -lws2_32 -loleaut32 -luxtheme -loleacc -lwinspool

# 静的リンクフラグ (単一EXE生成)
STATIC_FLAGS = -static -static-libgcc -static-libstdc++ -mwindows

CORE_SRCS = src/gsi_service.cpp \
            src/jma_service.cpp \
            src/jma_stations.cpp \
            src/http_transport.cpp \
            src/preset_store.cpp

CORE_HDRS = src/gsi_service.h \
            src/jma_service.h \
            src/jma_stations.h \
            src/http_transport.h \
            src/preset_store.h

GUI_SRCS = src/main.cpp \
           src/main_frame.cpp \
           src/map_panel.cpp \
           $(CORE_SRCS)

GUI_HDRS = src/main_frame.h \
           src/map_panel.h \
           $(CORE_HDRS)

RC_SRC = src/resource.rc
RES_OBJ = src/resource.o

TARGET = GeoBaro.exe
CLI_TARGET = test_cli.exe

all: $(TARGET)

$(RES_OBJ): $(RC_SRC) icons/app_icon.ico Makefile
	windres -i $(RC_SRC) -o $(RES_OBJ) --include-dir=. -I$(WX_DIR)/lib/gcc_lib/mswu -I$(WX_DIR)/include

$(TARGET): $(GUI_SRCS) $(GUI_HDRS) $(RES_OBJ)
	$(CXX) $(CXXFLAGS) $(WX_INC) $(GUI_SRCS) $(RES_OBJ) -o $(TARGET) $(WX_LIB_DIR) $(WX_LIBS) $(WIN_LIBS) $(STATIC_FLAGS)
	strip -s $(TARGET)

$(CLI_TARGET): src/test_cli.cpp $(CORE_SRCS) $(CORE_HDRS)
	$(CXX) -O2 -std=c++17 -Wall src/test_cli.cpp $(CORE_SRCS) -o $(CLI_TARGET) -lwininet -static -static-libgcc -static-libstdc++

clean:
	del /f /q $(TARGET) $(CLI_TARGET) src\resource.o
