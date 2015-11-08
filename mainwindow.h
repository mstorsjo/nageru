#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <string>
#include <vector>

class GLWidget;
class QResizeEvent;

namespace Ui {
class Display;
class MainWindow;
}  // namespace Ui

class Mixer;
class QPushButton;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow();
	void resizeEvent(QResizeEvent *event) override;
	void mixer_created(Mixer *mixer);

public slots:
	void transition_clicked(int transition_number);
	void channel_clicked(int channel_number);
	void wb_button_clicked(int channel_number);
	void set_transition_names(std::vector<std::string> transition_names);
	void cutoff_knob_changed(int value);
	void relayout();

private:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void set_white_balance(int channel_number, int x, int y);

	// Called from the mixer.
	void audio_level_callback(float level_lufs, float peak_db, float global_level_lufs, float range_low_lufs, float range_high_lufs, float auto_gain_staging_db);

	Ui::MainWindow *ui;
	QPushButton *transition_btn1, *transition_btn2, *transition_btn3;
	std::vector<Ui::Display *> previews;
	int current_wb_pick_display = -1;
};

extern MainWindow *global_mainwindow;

#endif
