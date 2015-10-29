#include "mainwindow.h"
#include <thread>
#include <vector>
#include <string>
#include <QSignalMapper>
#include <QMetaType>
#include <QShortcut>
#include <QResizeEvent>

#include "context.h"
#include "mixer.h"

#include "ui_mainwindow.h"

using namespace std;

Q_DECLARE_METATYPE(std::vector<std::string>);

MainWindow *global_mainwindow = nullptr;

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	ui->me_live->set_output(Mixer::OUTPUT_LIVE);
	ui->me_preview->set_output(Mixer::OUTPUT_PREVIEW);

	// TODO: Ask for the real number.
	ui->preview1->set_output(Mixer::OUTPUT_INPUT0);
	ui->preview2->set_output(Mixer::OUTPUT_INPUT1);
	ui->preview3->set_output(Mixer::OUTPUT_INPUT2);

	// Hook up the preview clicks.
	{
		QSignalMapper *mapper = new QSignalMapper(this);
		mapper->setMapping(ui->preview1, 0),
		mapper->setMapping(ui->preview2, 1);
		mapper->setMapping(ui->preview3, 2);
		connect(ui->preview1, SIGNAL(clicked()), mapper, SLOT(map()));
		connect(ui->preview2, SIGNAL(clicked()), mapper, SLOT(map()));
		connect(ui->preview3, SIGNAL(clicked()), mapper, SLOT(map()));

		connect(mapper, SIGNAL(mapped(int)), this, SLOT(channel_clicked(int)));
	}

	// Hook up the preview keyboard keys.
	{
		QSignalMapper *mapper = new QSignalMapper(this);
		QShortcut *shortcut1 = new QShortcut(QKeySequence(Qt::Key_1), this);
		connect(shortcut1, SIGNAL(activated()), mapper, SLOT(map()));
		QShortcut *shortcut2 = new QShortcut(QKeySequence(Qt::Key_2), this);
		connect(shortcut2, SIGNAL(activated()), mapper, SLOT(map()));
		QShortcut *shortcut3 = new QShortcut(QKeySequence(Qt::Key_3), this);
		connect(shortcut3, SIGNAL(activated()), mapper, SLOT(map()));
		mapper->setMapping(shortcut1, 0),
		mapper->setMapping(shortcut2, 1);
		mapper->setMapping(shortcut3, 2);

		connect(mapper, SIGNAL(mapped(int)), this, SLOT(channel_clicked(int)));
	}

	// Hook up the transition buttons.
	// TODO: Make them dynamic.
	{
		QSignalMapper *mapper = new QSignalMapper(this);
		mapper->setMapping(ui->transition_btn1, 0),
		mapper->setMapping(ui->transition_btn2, 1);
		mapper->setMapping(ui->transition_btn3, 2);
		connect(ui->transition_btn1, SIGNAL(clicked()), mapper, SLOT(map()));
		connect(ui->transition_btn2, SIGNAL(clicked()), mapper, SLOT(map()));
		connect(ui->transition_btn3, SIGNAL(clicked()), mapper, SLOT(map()));
		connect(mapper, SIGNAL(mapped(int)), this, SLOT(transition_clicked(int)));
	}

	// Aiee...
	transition_btn1 = ui->transition_btn1;
	transition_btn2 = ui->transition_btn2;
	transition_btn3 = ui->transition_btn3;
	qRegisterMetaType<std::vector<std::string>>("std::vector<std::string>");
	connect(ui->preview1, SIGNAL(transition_names_updated(std::vector<std::string>)),
	        this, SLOT(set_transition_names(std::vector<std::string>)));
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	// Ask for a relayout, but only after the event loop is done doing relayout
	// on everything else.
	QMetaObject::invokeMethod(this, "relayout", Qt::QueuedConnection);
}

void MainWindow::mixer_created(Mixer *mixer)
{
	mixer->set_audio_level_callback([this](float level_lufs, float peak_db){
		ui->vu_meter->set_level(level_lufs);

		char buf[256];
		snprintf(buf, sizeof(buf), "%.1f", peak_db);
		ui->peak_display->setText(buf);
		if (peak_db > -0.1f) {  // -0.1 dBFS is EBU peak limit.
			ui->peak_display->setStyleSheet("QLabel { background-color: red; color: white; }");
		} else {
			ui->peak_display->setStyleSheet("");
		}
	});
}

void MainWindow::relayout()
{
	int width = size().width();
	int height = size().height();

	// Allocate the height; the most important part is to keep the main displays
	// at 16:9 if at all possible.
	double me_width = ui->me_preview->width();
	double me_height = me_width * 9.0 / 16.0 + ui->label_preview->height();

	// TODO: Scale the widths when we need to do this.
	if (me_height / double(height) > 0.8) {
		me_height = height * 0.8;
	}

	// The previews will be constrained by the remaining height, and the width.
	// FIXME: spacing?
	double preview_height = std::min(height - me_height, (width / 4.0) * 9.0 / 16.0);

	ui->vertical_layout->setStretch(0, lrintf(me_height));
	ui->vertical_layout->setStretch(1, std::max<int>(1, lrintf(height - me_height - preview_height)));
	ui->vertical_layout->setStretch(2, lrintf(preview_height));

	// Set the widths for the previews.
	double preview_width = preview_height * 16.0 / 9.0;  // FIXME: spacing?

	ui->preview_displays->setStretch(0, lrintf(preview_width));
	ui->preview_displays->setStretch(1, lrintf(preview_width));
	ui->preview_displays->setStretch(2, lrintf(preview_width));
	ui->preview_displays->setStretch(3, lrintf(preview_width));
	ui->preview_displays->setStretch(4, lrintf(width - 4.0 * preview_width));
}

void MainWindow::set_transition_names(vector<string> transition_names)
{
	if (transition_names.size() < 1) {
		transition_btn1->setText(QString(""));
	} else {
		transition_btn1->setText(QString::fromStdString(transition_names[0]));
	}
	if (transition_names.size() < 2) {
		transition_btn2->setText(QString(""));
	} else {
		transition_btn2->setText(QString::fromStdString(transition_names[1]));
	}
	if (transition_names.size() < 3) {
		transition_btn3->setText(QString(""));
	} else {
		transition_btn3->setText(QString::fromStdString(transition_names[2]));
	}
}

void MainWindow::transition_clicked(int transition_number)
{
	global_mixer->transition_clicked(transition_number);
}

void MainWindow::channel_clicked(int channel_number)
{
	global_mixer->channel_clicked(channel_number);
}
