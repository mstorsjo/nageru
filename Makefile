CXX=g++
PKG_MODULES = Qt5Core Qt5Gui Qt5Widgets Qt5OpenGLExtensions Qt5OpenGL libusb-1.0 movit lua5.2 libmicrohttpd
CXXFLAGS := -O2 -march=native -g -std=gnu++11 -Wall -Wno-deprecated-declarations -Werror -fPIC $(shell pkg-config --cflags $(PKG_MODULES)) -pthread -DMOVIT_SHADER_DIR=\"$(shell pkg-config --variable=shaderdir movit)\"
LDFLAGS=$(shell pkg-config --libs $(PKG_MODULES)) -lEGL -lGL -pthread -lva -lva-drm -lva-x11 -lX11 -lavformat -lavcodec -lavutil -lzita-resampler -lebur128

# Qt objects
OBJS=glwidget.o main.o mainwindow.o vumeter.o
OBJS += glwidget.moc.o mainwindow.moc.o vumeter.moc.o

# Mixer objects
OBJS += h264encode.o mixer.o bmusb/bmusb.o pbo_frame_allocator.o context.o ref_counted_frame.o theme.o resampler.o httpd.o

%.o: %.cpp
	$(CXX) -MMD -MP $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<

%.h: %.ui
	uic $< -o $@

%.moc.cpp: %.h
	moc $< -o $@

all: nageru

nageru: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

mainwindow.o: mainwindow.cpp ui_mainwindow.h

DEPS=$(OBJS:.o=.d)
-include $(DEPS)

clean:
	$(RM) $(OBJS) $(DEPS) nageru ui_mainwindow.h glwidget.moc.cpp mainwindow.moc.cpp window.moc.cpp chain-*.frag *.dot
