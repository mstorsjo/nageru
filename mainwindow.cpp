#include "mainwindow.h"
#include "window.h"
#include <thread>

#include "context.h"
#include "mixer.h"

#include "ui_mainwindow.cpp"

using std::thread;

MainWindow::MainWindow()
{
	Ui::MainWindow *window = new Ui::MainWindow;
	window->setupUi(this);
}
