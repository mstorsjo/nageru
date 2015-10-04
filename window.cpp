#include "window.h"

#include <QBoxLayout>
#include <QString>

#include "glwidget.h"

Window::Window(MainWindow *mw)
	: main_window(mw)
{
	gl_widget = new GLWidget;

	QVBoxLayout *mainLayout = new QVBoxLayout;
	QHBoxLayout *container = new QHBoxLayout;
	container->addWidget(gl_widget);

	QWidget *w = new QWidget;
	w->setLayout(container);
	mainLayout->addWidget(w);

	setLayout(mainLayout);

	setWindowTitle(tr("Nageru"));
}
