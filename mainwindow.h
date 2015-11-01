#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <string>
#include <vector>

class QResizeEvent;

namespace Ui {
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
	void set_transition_names(std::vector<std::string> transition_names);
	void relayout();

private:
	Ui::MainWindow *ui;
	QPushButton *transition_btn1, *transition_btn2, *transition_btn3;
};

extern MainWindow *global_mainwindow;

#endif
