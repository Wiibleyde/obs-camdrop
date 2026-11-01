#pragma once

#include <QWidget>

#include <string>

class QLabel;
class QPushButton;
class QStackedLayout;
class QCefWidget;
struct QCef;

namespace camdrop::ui {

// Supervision dock: the /dock page of the plugin shown through OBS's browser panel (CEF), with a
// native placeholder while the server is off or when obs-browser is not available.
void create_dock();

class SupervisionDock : public QWidget {
	Q_OBJECT

public:
	explicit SupervisionDock(QWidget *parent);
	~SupervisionDock() override;
	void close_browser();

public slots:
	void refresh();

private:
	QStackedLayout *stack_;
	QWidget *placeholder_;
	QLabel *message_;
	QPushButton *open_browser_;
	QCef *cef_ = nullptr;
	QCefWidget *browser_ = nullptr;
	std::string url_;
	int listener_id_ = 0;
};

} // namespace camdrop::ui
