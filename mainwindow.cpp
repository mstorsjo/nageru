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
#include <QSize>
#include <QString>

#include "glwidget.h"
#include "lrameter.h"
#include "mixer.h"
#include "post_to_main_thread.h"
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
	connect(ui->transition_btn1, &QPushButton::clicked, std::bind(&MainWindow::transition_clicked, this, 0));
	connect(ui->transition_btn2, &QPushButton::clicked, std::bind(&MainWindow::transition_clicked, this, 1));
	connect(ui->transition_btn3, &QPushButton::clicked, std::bind(&MainWindow::transition_clicked, this, 2));

	// Aiee...
	transition_btn1 = ui->transition_btn1;
	transition_btn2 = ui->transition_btn2;
	transition_btn3 = ui->transition_btn3;
	qRegisterMetaType<std::vector<std::string>>("std::vector<std::string>");
	connect(ui->me_preview, &GLWidget::transition_names_updated, this, &MainWindow::set_transition_names);
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

	for (unsigned i = 0; i < num_previews; ++i) {
		Mixer::Output output = Mixer::Output(Mixer::OUTPUT_INPUT0 + i);

		QWidget *preview = new QWidget(this);
		Ui::Display *ui_display = new Ui::Display;
		ui_display->setupUi(preview);
		ui_display->label->setText(mixer->get_channel_name(output).c_str());
		ui_display->display->set_output(output);
		ui->preview_displays->insertWidget(previews.size(), preview, 1);
		previews.push_back(ui_display);

		// Hook up the click.
		connect(ui_display->display, &GLWidget::clicked, std::bind(&MainWindow::channel_clicked, this, i));

		// Hook up the keyboard key.
		QShortcut *shortcut = new QShortcut(QKeySequence(Qt::Key_1 + i), this);
		connect(shortcut, &QShortcut::activated, std::bind(&MainWindow::channel_clicked, this, i));

		// Hook up the white balance button (irrelevant if invisible).
		ui_display->wb_button->setVisible(mixer->get_supports_set_wb(output));
		connect(ui_display->wb_button, &QPushButton::clicked, std::bind(&MainWindow::wb_button_clicked, this, i));
	}

	connect(ui->locut_cutoff_knob, &QDial::valueChanged, [this](int value) {
		float octaves = value * 0.1f;
		float cutoff_hz = 20.0 * pow(2.0, octaves);
		global_mixer->set_locut_cutoff(cutoff_hz);

		char buf[256];
		snprintf(buf, sizeof(buf), "%ld Hz", lrintf(cutoff_hz));
		ui->locut_cutoff_display->setText(buf);
	});

	mixer->set_audio_level_callback([this](float level_lufs, float peak_db, float global_level_lufs, float range_low_lufs, float range_high_lufs, float auto_gain_staging_db){
		post_to_main_thread([=]() {
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

			ui->gainstaging_knob->setValue(lrintf(auto_gain_staging_db * 10.0f));
			snprintf(buf, sizeof(buf), "%+.1f dB", auto_gain_staging_db);
			ui->gainstaging_db_display->setText(buf);
		});
	});
}

void MainWindow::relayout()
{
	int height = ui->vertical_layout->geometry().height();

	double remaining_height = height;

	// Allocate the height; the most important part is to keep the main displays
	// at 16:9 if at all possible.
	double me_width = ui->me_preview->width();
	double me_height = me_width * 9.0 / 16.0 + ui->label_preview->height() + ui->preview_vertical_layout->spacing();

	// TODO: Scale the widths when we need to do this.
	if (me_height / double(height) > 0.8) {
		me_height = height * 0.8;
	}
	remaining_height -= me_height + ui->vertical_layout->spacing();

	double audiostrip_height = ui->audiostrip->geometry().height();
	remaining_height -= audiostrip_height + ui->vertical_layout->spacing();

	// The previews will be constrained by the remaining height, and the width.
	double preview_label_height = previews[0]->title_bar->geometry().height() + ui->preview_displays->spacing();  // Wrong spacing?
	int preview_total_width = ui->preview_displays->geometry().width();
	double preview_height = std::min(remaining_height - preview_label_height, (preview_total_width / double(previews.size())) * 9.0 / 16.0);
	remaining_height -= preview_height + preview_label_height + ui->vertical_layout->spacing();

	ui->vertical_layout->setStretch(0, lrintf(me_height));
	ui->vertical_layout->setStretch(1, 0);  // Don't stretch the audiostrip.
	ui->vertical_layout->setStretch(2, std::max<int>(1, remaining_height));  // Spacer.
	ui->vertical_layout->setStretch(3, lrintf(preview_height + preview_label_height));

	// Set the widths for the previews.
	double preview_width = preview_height * 16.0 / 9.0;
	double remaining_preview_width = preview_total_width;

	for (unsigned i = 0; i < previews.size(); ++i) {
		ui->preview_displays->setStretch(i, lrintf(preview_width));
		remaining_preview_width -= preview_width + ui->preview_displays->spacing();
	}

	// The preview horizontal spacer.
	ui->preview_displays->setStretch(previews.size(), lrintf(remaining_preview_width));
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
	if (current_wb_pick_display == channel_number) {
		// The picking was already done from eventFilter(), since we don't get
		// the mouse pointer here.
	} else {
		global_mixer->channel_clicked(channel_number);
	}
}

void MainWindow::wb_button_clicked(int channel_number)
{
	current_wb_pick_display = channel_number;
	QApplication::setOverrideCursor(Qt::CrossCursor);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (current_wb_pick_display != -1 &&
	    event->type() == QEvent::MouseButtonRelease &&
	    watched->isWidgetType()) {
		QApplication::restoreOverrideCursor();
		if (watched == previews[current_wb_pick_display]->display) {
			const QMouseEvent *mouse_event = (QMouseEvent *)event;
			set_white_balance(current_wb_pick_display, mouse_event->x(), mouse_event->y());
		} else {
			// The user clicked on something else, give up.
			// (The click goes through, which might not be ideal, but, yes.)
			current_wb_pick_display = -1;
		}
	}
	return false;
}

namespace {

double srgb_to_linear(double x)
{
	if (x < 0.04045) {
		return x / 12.92;
	} else {
		return pow((x + 0.055) / 1.055, 2.4);
	}
}

}  // namespace

void MainWindow::set_white_balance(int channel_number, int x, int y)
{
	// Set the white balance to neutral for the grab. It's probably going to
	// flicker a bit, but hopefully this display is not live anyway.
	global_mixer->set_wb(Mixer::OUTPUT_INPUT0 + channel_number, 0.5, 0.5, 0.5);
	previews[channel_number]->display->updateGL();
	QRgb reference_color = previews[channel_number]->display->grabFrameBuffer().pixel(x, y);

	double r = srgb_to_linear(qRed(reference_color) / 255.0);
	double g = srgb_to_linear(qGreen(reference_color) / 255.0);
	double b = srgb_to_linear(qBlue(reference_color) / 255.0);
	global_mixer->set_wb(Mixer::OUTPUT_INPUT0 + channel_number, r, g, b);
	previews[channel_number]->display->updateGL();
}
