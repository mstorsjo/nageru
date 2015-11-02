#include "mainwindow.h"

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <string>
#include <vector>
#include <QBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QSignalMapper>
#include <QSize>
#include <QString>

#include "glwidget.h"
#include "lrameter.h"
#include "mixer.h"
#include "ui_display.h"
#include "ui_mainwindow.h"
#include "vumeter.h"

class QResizeEvent;

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
	connect(ui->me_preview, SIGNAL(transition_names_updated(std::vector<std::string>)),
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
	// Make the previews.
	unsigned num_previews = mixer->get_num_channels();
	QSignalMapper *mapper = new QSignalMapper(this);

	for (unsigned i = 0; i < num_previews; ++i) {
		Mixer::Output output = Mixer::Output(Mixer::OUTPUT_INPUT0 + i);

		QWidget *preview = new QWidget(this);
		Ui::Display *ui_display = new Ui::Display;
		ui_display->setupUi(preview);
		ui_display->label->setText(mixer->get_channel_name(output).c_str());
		ui_display->wb_button->setVisible(mixer->get_supports_set_wb(output));
		ui_display->display->set_output(output);
		ui->preview_displays->insertWidget(previews.size(), preview, 1);
		previews.push_back(ui_display);

		// Hook up the click.
		mapper->setMapping(ui_display->display, i);
		connect(ui_display->display, SIGNAL(clicked()), mapper, SLOT(map()));

		// Hook up the keyboard key.
		QShortcut *shortcut = new QShortcut(QKeySequence(Qt::Key_1 + i), this);
		mapper->setMapping(shortcut, i);
		connect(shortcut, SIGNAL(activated()), mapper, SLOT(map()));
	}

	connect(mapper, SIGNAL(mapped(int)), this, SLOT(channel_clicked(int)));

	mixer->set_audio_level_callback([this](float level_lufs, float peak_db, float global_level_lufs, float range_low_lufs, float range_high_lufs){
		ui->vu_meter->set_level(level_lufs);
		ui->lra_meter->set_levels(global_level_lufs, range_low_lufs, range_high_lufs);

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
	double preview_label_height = previews[0]->label->height();
	double preview_height = std::min(height - me_height - preview_label_height, (width / double(previews.size())) * 9.0 / 16.0);

	ui->vertical_layout->setStretch(0, lrintf(me_height));
	ui->vertical_layout->setStretch(1, std::max<int>(1, lrintf(height - me_height - preview_height)));
	ui->vertical_layout->setStretch(2, lrintf(preview_height + preview_label_height));

	// Set the widths for the previews.
	double preview_width = preview_height * 16.0 / 9.0;  // FIXME: spacing?

	for (unsigned i = 0; i < previews.size(); ++i) {
		ui->preview_displays->setStretch(i, lrintf(preview_width));
	}

	// The spacer.
	ui->preview_displays->setStretch(previews.size(), lrintf(width - previews.size() * preview_width));
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
