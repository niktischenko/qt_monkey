//#define DEBUG_ANALYZER
#include "user_events_analyzer.hpp"

#include <cassert>
#include <cstring>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QTreeWidget>
#include <QWidget>
#include <QtCore/QEvent>
#include <QtGui/QMouseEvent>

#include "common.hpp"

using qt_monkey_agent::UserEventsAnalyzer;
using qt_monkey_agent::GenerateCommand;
using qt_monkey_agent::TreeWidgetWatcher;

#ifdef DEBUG_ANALYZER
#define DBGPRINT(fmt, ...) qDebug(fmt, __VA_ARGS__)
#else
#define DBGPRINT(fmt, ...)                                                     \
    do {                                                                       \
    } while (false)
#endif

namespace
{
static QString numAmongOthersWithTheSameClass(const QObject &w)
{
    QObject *p = w.parent();
    if (p == nullptr)
        return QString();

    const QObjectList &childs = p->children();
    int order = 0;
    for (QObject *obj : childs) {
        if (obj == &w) {
            if (order == 0)
                return QString();
            else
                return QString(",%1").arg(order);
            continue;
        }
        if (std::strcmp(obj->metaObject()->className(),
                        w.metaObject()->className())
            == 0)
            ++order;
    }
    return QString();
}

static QString qtObjectId(const QObject &w)
{
    const QString name = w.objectName();
    if (name.isEmpty()) {
        return QString("<class_name=%1%2>")
            .arg(w.metaObject()->className())
            .arg(numAmongOthersWithTheSameClass(w));
    }
    return name;
}

static QString mouseEventToJavaScript(const QString &widgetName,
                                      QMouseEvent *mouseEvent,
                                      const QPoint &pos)
{
    const QString mouseBtn
        = qt_monkey_agent::mouseButtonEnumToString(mouseEvent->button());

    if (mouseEvent->type() == QEvent::MouseButtonDblClick)
        return QString("Test.mouseDClick('%1', '%2', %3, %4);")
            .arg(widgetName)
            .arg(mouseBtn)
            .arg(pos.x())
            .arg(pos.y());
    else
        return QString("Test.mouseClick('%1', '%2', %3, %4);")
            .arg(widgetName)
            .arg(mouseBtn)
            .arg(pos.x())
            .arg(pos.y());
}

static bool isOnlyOneChildWithSuchClass(QObject &w)
{
    if (w.parent() == nullptr)
        return false;

    const QObjectList &childs = w.parent()->children();

    for (QObject *obj : childs)
        if (obj != &w
            && std::strcmp(obj->metaObject()->className(),
                           w.metaObject()->className())
                   == 0)
            return false;
    return true;
}

static QString qmenuActivateClick(QObject *, QEvent *event,
                                  const std::pair<QWidget *, QString> &widget,
                                  const GenerateCommand &)
{
    QString res;
    if (widget.first == nullptr || event == nullptr
        || !(event->type() == QEvent::MouseButtonDblClick
             || event->type() == QEvent::MouseButtonPress)
        || std::strcmp(widget.first->metaObject()->className(), "QMenu") != 0)
        return res;
    auto qm = qobject_cast<QMenu *>(widget.first);
    QAction *act = qm->actionAt(widget.first->mapFromGlobal(
        static_cast<QMouseEvent *>(event)->globalPos()));
    if (act != nullptr) {
        DBGPRINT("%s: act->text() %s", Q_FUNC_INFO, qPrintable(act->text()));
        if (!widget.first->objectName()
                 .isEmpty() /*widgetName != "<unknown name>"*/)
            res = QString("Test.activateItem('%1', '%2');")
                      .arg(widget.second)
                      .arg(act->text());
        else
            res = QString("Test.activateMenuItem('%1');").arg(act->text());
    }
    return res;
}

static QWidget *searchThroghSuperClassesAndParents(QWidget *widget,
                                                   const char *wname,
                                                   size_t limit = size_t(-1))
{
    DBGPRINT("%s: begin: wname %s", Q_FUNC_INFO, wname);
    for (size_t i = 0; widget != nullptr && i < limit; ++i) {
        const QMetaObject *mo = widget->metaObject();
        while (mo && std::strcmp(mo->className(), wname) != 0) {
            mo = mo->superClass();
        }

        if (mo != nullptr) {
            DBGPRINT("%s: yeah, it(%s) is %s", Q_FUNC_INFO,
                     qPrintable(widget->objectName()), wname);
            return widget;
        } else {
            widget = qobject_cast<QWidget *>(widget->parent());
        }
    }
    return nullptr;
}

static QString
qtreeWidgetActivateClick(QObject *, QEvent *event,
                         const std::pair<QWidget *, QString> &widget,
                         const GenerateCommand &asyncCodeGen)
{
    static TreeWidgetWatcher treeWidgetWatcher(asyncCodeGen);

    QString res;
    if (widget.first == nullptr || event == nullptr
        || !(event->type() == QEvent::MouseButtonDblClick
             || event->type() == QEvent::MouseButtonPress))
        return res;

    auto mouseEvent = static_cast<QMouseEvent *>(event);
    const QPoint pos = widget.first->mapFromGlobal(mouseEvent->globalPos());

    QWidget *treeWidget
        = searchThroghSuperClassesAndParents(widget.first, "QTreeWidget", 2);

    if (widget.first != treeWidget
        && qobject_cast<QWidget *>(widget.first->parent()) != treeWidget)
        return res;

    DBGPRINT("%s: yeah, it is tree widget", Q_FUNC_INFO);
    QTreeWidget *tw = qobject_cast<QTreeWidget *>(treeWidget);
    assert(tw != nullptr);
    QTreeWidgetItem *twi = tw->itemAt(pos);
#ifdef DEBUG_ANALYZER
    QRect tir =  tw->visualItemRect(twi);
#endif
    DBGPRINT("%s: tir.x %d, tir.y %d, pos.x %d, pos.y %d, %s", Q_FUNC_INFO,
             tir.x(), tir.y(), pos.x(), pos.y(),
             mouseEvent->type() == QEvent::MouseButtonDblClick ? "double click"
                                                               : "click");
    if (twi != nullptr) {
        QString text = twi->text(0);
        if (!text.isEmpty()) {
            text.replace(QChar('\n'), "\\n");
            DBGPRINT("%s: QtreeWidget text %s", Q_FUNC_INFO, qPrintable(text));
            if (mouseEvent->type() == QEvent::MouseButtonDblClick)
                res = QStringLiteral("Test.doubleClickItem('%1', '%2');")
                          .arg(qt_monkey_agent::fullQtWidgetId(*tw))
                          .arg(text);
            else
                res = QStringLiteral("Test.activateItem('%1', '%2');")
                          .arg(qt_monkey_agent::fullQtWidgetId(*tw))
                          .arg(text);

            if (treeWidgetWatcher.watch(tw)) {//new one
                QObject::connect(tw, SIGNAL(itemExpanded(QTreeWidgetItem *)), &treeWidgetWatcher,
                        SLOT(itemExpanded(QTreeWidgetItem *)));
                QObject::connect(tw, SIGNAL(destroyed(QObject *)), &treeWidgetWatcher,
                        SLOT(treeWidgetDestroyed(QObject *)));
            }
        }
    }
    return res;
}

#if 0
    static QString qComboBoxActivateClick()
    {
if (QWidget *combobox =
		   search_throgh_super_classes_and_parents(widget, "QComboBox")) {
		DBGPRINT("%s: this is combobox", Q_FUNC_INFO);
		if (widget == combobox)
			return Unknown;
		QListView *lv = qobject_cast<QListView *>(widget);
		if (lv == nullptr && widget->parent() != nullptr)
			lv = qobject_cast<QListView *>(widget->parent());
		if (lv == nullptr)
			return Unknown;
		DBGPRINT("%s catch press on QListView falldown list",
		       Q_FUNC_INFO);
		QModelIndex idx = lv->indexAt(pos);
		DBGPRINT("%s: row %d", Q_FUNC_INFO, idx.row());
		scriptLine = QString("Test.activateItem('%1', '%2');")
			.arg(full_qt_widget_id(combobox))
			.arg(qobject_cast<QComboBox *>(combobox)->itemText(idx.row()));
		return CodeGenerated;
	} else if (QWidget *alistwdg = search_throgh_super_classes_and_parents(widget, "QListWidget")) {
		DBGPRINT("%s: this is QListWidget", Q_FUNC_INFO);
		QListWidget *listwdg = qobject_cast<QListWidget *>(alistwdg);
		if (listwdg == nullptr)
			return Unknown;
		QListWidgetItem *it = listwdg->itemAt(pos);
		if (it == nullptr)
			return Unknown;
		scriptLine = QString("Test.activateItem('%1', '%2');")
			.arg(full_qt_widget_id(listwdg)).arg(it->text());
		return CodeGenerated;
    }
    }
static QString qListWidgetActivateClick()
{
if (QWidget *alistwdg = search_throgh_super_classes_and_parents(widget, "QListWidget")) {
		DBGPRINT("%s: this is QListWidget", Q_FUNC_INFO);
		QListWidget *listwdg = qobject_cast<QListWidget *>(alistwdg);
		if (listwdg == nullptr)
			return Unknown;
		QListWidgetItem *it = listwdg->itemAt(pos);
		if (it == nullptr)
			return Unknown;
		scriptLine = QString("Test.activateItem('%1', '%2');")
			.arg(full_qt_widget_id(listwdg)).arg(it->text());
		return CodeGenerated;
	}
}

    static QString qTreeViewActivateClick()
    {
if (QWidget *tree_view =
		   search_throgh_super_classes_and_parents(widget, "QTreeView", 2)) {
		if (widget == tree_view ||
		    qobject_cast<QWidget *>(widget->parent()) == tree_view) {
			DBGPRINT("%s: yeah, it is tree view", Q_FUNC_INFO);
			QTreeView *tv = qobject_cast<QTreeView *>(tree_view);

			QModelIndex mi = tv->indexAt(pos);
			if (mi.isValid()) {
				DBGPRINT("%s: column %d, row %d, have parent %s", Q_FUNC_INFO,
				       mi.column(), mi.row(), mi.parent() == QModelIndex() ? "false" : "true");
				if (mouseEvent->type() == QEvent::MouseButtonDblClick)
					scriptLine = QString("Test.doubleClickOnItemInView('%1', %2);")
						.arg(full_qt_widget_id(tv)).arg(model_index_to_pos(mi));
				else
					scriptLine = QString("Test.activateItemInView('%1', %2);")
						.arg(full_qt_widget_id(tv)).arg(model_index_to_pos(mi));

				if (!treeViewSet_.contains(tv)) {
					treeViewSet_ += tv;
					model_to_view_[tv->model()] = tv;
					connect(tv, SIGNAL(expanded(const QModelIndex&)),
						this, SLOT(treeViewExpanded(const QModelIndex &)));
					connect(tv, SIGNAL(destroyed(QObject *)),
						this, SLOT(treeViewDestroyed(QObject *)));
				}

				return CodeGenerated;
			} else {
				DBGPRINT("%s: not valid model index for tv", Q_FUNC_INFO);
            }
		}
	}
    }

    static QString qListViewActivateClick()
    {
 if (QWidget *list_view = search_throgh_super_classes_and_parents(widget, "QListView", 2)) {
        if (widget == list_view || qobject_cast<QWidget *>(widget->parent()) == list_view) {
            DBGPRINT("%s: yeah, it is list view", Q_FUNC_INFO);
            QListView *lv = qobject_cast<QListView *>(list_view);
			QModelIndex mi = lv->indexAt(pos);
            if (mi.isValid() && !mi.data().toString().isEmpty()) {
				DBGPRINT("%s: column %d, row %d, have parent %s", Q_FUNC_INFO,
				       mi.column(), mi.row(), mi.parent() == QModelIndex() ? "false" : "true");
                QString text = mi.data().toString();
                text.replace(QChar('\n'), "\\n");
                text.replace(QChar('\''), "\\'");
                scriptLine = QString("Test.activateItem('%1', '%2');")
                    .arg(full_qt_widget_id(lv)).arg(text);

				return CodeGenerated;
			} else {
				DBGPRINT("%s: not valid model index for lv", Q_FUNC_INFO);
            }
        }
    }
    }
#endif
static const std::pair<Qt::MouseButton, QLatin1String> mouseBtnNames[] = {
    {Qt::LeftButton, QLatin1String("Qt.LeftButton")},
    {Qt::RightButton, QLatin1String("Qt.RightButton")},
    {Qt::MidButton, QLatin1String("Qt.MidButton")},
};
}

bool qt_monkey_agent::stringToMouseButton(const QString &str,
                                          Qt::MouseButton &bt)
{
    for (auto &&elm : mouseBtnNames)
        if (elm.second == str) {
            bt = elm.first;
            return true;
        }

    return false;
}

QString qt_monkey_agent::mouseButtonEnumToString(Qt::MouseButton b)
{

    for (auto &&elm : mouseBtnNames)
        if (elm.first == b)
            return elm.second;

    return QLatin1String("<unknown button>");
}

QString qt_monkey_agent::fullQtWidgetId(const QWidget &w)
{
    QString res = qtObjectId(w);
    DBGPRINT("%s: class name %s, id %s", Q_FUNC_INFO,
             w.metaObject()->className(), qPrintable(res));
    QObject *cur_obj = w.parent();
    while (cur_obj != nullptr) {
        res = qtObjectId(*cur_obj) + "." + res;
        cur_obj = cur_obj->parent();
    }
    return res;
}

UserEventsAnalyzer::UserEventsAnalyzer(
    std::list<CustomEventAnalyzer> customEventAnalyzers, QObject *parent)
    : QObject(parent), customEventAnalyzers_(std::move(customEventAnalyzers)),
      generateScriptCmd_(
          [this](QString code) { emit userEventInScriptForm(code); })
{
    customEventAnalyzers_.emplace_back(qmenuActivateClick);
    customEventAnalyzers_.emplace_back(qtreeWidgetActivateClick);
}

QString UserEventsAnalyzer::callCustomEventAnalyzers(
    QObject *obj, QEvent *event,
    const std::pair<QWidget *, QString> &widget) const
{
    QString code;
    for (auto &&userCustomAnalyzer : customEventAnalyzers_) {
        code = userCustomAnalyzer(obj, event, widget, generateScriptCmd_);
        if (!code.isEmpty())
            return code;
    }
    return code;
}

bool UserEventsAnalyzer::eventFilter(QObject *obj, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        DBGPRINT("%s: key event for '%s'\n", Q_FUNC_INFO,
                 qPrintable(obj->objectName()));
        break;
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonPress: {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        DBGPRINT("%s: mouse event for '%s': %s\n", Q_FUNC_INFO,
                 qPrintable(obj->objectName()),
                 event->type() == QEvent::MouseButtonDblClick
                     ? "double click"
                     : "release event");
        QWidget *w = QApplication::widgetAt(mouseEvent->globalPos());
        if (w == nullptr) {
#ifdef DEBUG_ANALYZER
            QPoint p = mouseEvent->globalPos();
#endif
            DBGPRINT(
                "(%s, %d): Can not find out what widget is used(x %d, y %d)!!!",
                Q_FUNC_INFO, __LINE__, p.x(), p.y());
            return false;
        }

        QPoint pos = w->mapFromGlobal(mouseEvent->globalPos());
        QString widgetName = fullQtWidgetId(*w);
        QString scriptLine
            = callCustomEventAnalyzers(obj, event, {w, widgetName});
        if (scriptLine.isEmpty())
            scriptLine = mouseEventToJavaScript(widgetName, mouseEvent, pos);

        if (w->objectName().isEmpty() && !isOnlyOneChildWithSuchClass(*w)) {
            QWidget *baseWidget = w;
            while (w != nullptr && w->objectName().isEmpty())
                w = qobject_cast<QWidget *>(w->parent());
            if (w != nullptr && w != baseWidget) {
                pos = w->mapFromGlobal(mouseEvent->globalPos());
                widgetName = fullQtWidgetId(*w);
                QString anotherScript
                    = callCustomEventAnalyzers(obj, event, {w, widgetName});
                if (anotherScript.isEmpty())
                    anotherScript
                        = mouseEventToJavaScript(widgetName, mouseEvent, pos);
                if (scriptLine != anotherScript)
                    scriptLine = QString("%1\n//%2")
                                     .arg(scriptLine)
                                     .arg(anotherScript);
            }
        }
        DBGPRINT("%s: emit userEventInScriptForm", Q_FUNC_INFO);
        emit userEventInScriptForm(scriptLine);
        break;
    } // event by mouse
    default: {
        const QString code
            = callCustomEventAnalyzers(obj, event, {nullptr, QString()});
        if (!code.isEmpty())
            emit userEventInScriptForm(code);
        break;
    }
    } // switch (event->type())
    return QObject::eventFilter(obj, event);
}

void TreeWidgetWatcher::itemExpanded(QTreeWidgetItem *twi)
{
	DBGPRINT("%s begin", Q_FUNC_INFO);
    assert(twi != nullptr);
	QTreeWidget *tw = twi->treeWidget();
    assert(tw != nullptr);
	generateScriptCmd_(QStringLiteral("Test.expandItemInTree('%1', '%2');")
                       .arg(fullQtWidgetId(*tw)).arg(twi->text(0)));

	disconnect(tw, SIGNAL(itemExpanded(QTreeWidgetItem *)),
               this, SLOT(itemExpanded(QTreeWidgetItem *)));
    auto it = treeWidgetsSet_.find(tw);
	assert(it != treeWidgetsSet_.end());
	treeWidgetsSet_.erase(it);
}

void TreeWidgetWatcher::treeWidgetDestroyed(QObject *obj)
{
	DBGPRINT("begin %s", Q_FUNC_INFO);
	assert(obj != nullptr);
    auto it = treeWidgetsSet_.find(obj);
	if (it != treeWidgetsSet_.end())
		treeWidgetsSet_.erase(it);
}

bool TreeWidgetWatcher::watch(QTreeWidget *tw)
{
    assert(tw != nullptr);
    return treeWidgetsSet_.insert(tw).second;
}
