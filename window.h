#ifndef WINDOW_H
#define WINDOW_H

#include <QWidget>

class GLWidget;
class MainWindow;

class Window : public QWidget
{
	Q_OBJECT

public:
	Window(MainWindow *mw);

private:
	GLWidget *gl_widget;
	MainWindow *main_window;
};

#endif
