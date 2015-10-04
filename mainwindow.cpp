#include "mainwindow.h"
#include "window.h"
#include <thread>

#include "context.h"
#include "mixer.h"

#include "ui_mainwindow.cpp"

using std::thread;

MainWindow::MainWindow()
{
	Ui::MainWindow *ui = new Ui::MainWindow;
	ui->setupUi(this);
	connect(ui->cut_btn, SIGNAL(clicked()), this, SLOT(cut()));
}

void MainWindow::cut()
{
	static int i = 0;
	mixer_cut(Source((++i) % 3));
}
