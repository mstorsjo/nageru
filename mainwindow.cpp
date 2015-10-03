#include "mainwindow.h"
#include "window.h"
#include <thread>

#include "context.h"
#include "mixer.h"

using std::thread;

MainWindow::MainWindow()
{
        setCentralWidget(new Window(this));
}
