#include "aboutdialog.h"

#include "ui_aboutdialog.h"

using namespace std;

AboutDialog::AboutDialog()
	: ui(new Ui::AboutDialog)
{
	ui->setupUi(this);
}

