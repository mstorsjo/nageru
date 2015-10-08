#include "mainwindow.h"
#include "window.h"
#include <thread>
#include <QSignalMapper>

#include "context.h"
#include "mixer.h"

#include "ui_mainwindow.cpp"

using std::thread;

MainWindow::MainWindow()
{
	Ui::MainWindow *ui = new Ui::MainWindow;
	ui->setupUi(this);
	connect(ui->cut_btn, SIGNAL(clicked()), this, SLOT(cut()));

	ui->me_live->set_output(Mixer::OUTPUT_LIVE);
	ui->me_preview->set_output(Mixer::OUTPUT_PREVIEW);

	// TODO: Ask for the real number.
	ui->preview1->set_output(Mixer::OUTPUT_INPUT0);
	ui->preview2->set_output(Mixer::OUTPUT_INPUT1);
	ui->preview3->set_output(Mixer::OUTPUT_INPUT2);

	QSignalMapper *mapper = new QSignalMapper(this);
	mapper->setMapping(ui->preview1, 0),
	mapper->setMapping(ui->preview2, 1);
	mapper->setMapping(ui->preview3, 2);
	connect(ui->preview1, SIGNAL(clicked()), mapper, SLOT(map()));
	connect(ui->preview2, SIGNAL(clicked()), mapper, SLOT(map()));
	connect(ui->preview3, SIGNAL(clicked()), mapper, SLOT(map()));
	connect(mapper, SIGNAL(mapped(int)), this, SLOT(channel_clicked(int)));
}

void MainWindow::cut()
{
	global_mixer->transition_clicked(0);  // FIXME: real value
}

void MainWindow::channel_clicked(int channel_number)
{
	global_mixer->channel_clicked(channel_number);
}
