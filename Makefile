CXX=g++
CXXFLAGS := -O2 -march=native -g -std=gnu++11 -Wall -Wno-deprecated-declarations -fPIC $(shell pkg-config --cflags Qt5Core Qt5Widgets Qt5OpenGLExtensions libusb-1.0 movit) -pthread
LDFLAGS=$(shell pkg-config --libs Qt5Core Qt5Widgets Qt5OpenGLExtensions libusb-1.0 movit) -lEGL -lGL -pthread -lva -lva-drm -lva-x11 -lX11 -lavformat -lavcodec -lavutil

# Qt objects
OBJS=glwidget.o main.o mainwindow.o window.o
OBJS += glwidget.moc.o mainwindow.moc.o window.moc.o

# Mixer objects
OBJS += h264encode.o mixer.o bmusb.o pbo_frame_allocator.o context.o

%.moc.cpp: %.h
	moc $< -o $@

all: nageru

nageru: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^


