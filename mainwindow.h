#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <vector>
#include <string>

class QPushButton;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow();

public slots:
	void transition_clicked(int transition_number);
	void channel_clicked(int channel_number);
	void set_transition_names(std::vector<std::string> transition_names);

private:
	QPushButton *transition_btn1, *transition_btn2, *transition_btn3;
};

#endif
