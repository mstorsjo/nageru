#ifndef ABOUTWINDOW_H
#define ABOUTWINDOW_H

#include <QDialog>
#include <string>
#include <vector>
#include <sys/time.h>

#include "mixer.h"

namespace Ui {
class AboutDialog;
}  // namespace Ui

class AboutDialog : public QDialog
{
	Q_OBJECT

public:
	AboutDialog();

private:
	Ui::AboutDialog *ui;
};

#endif
