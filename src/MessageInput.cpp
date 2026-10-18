#include "MessageInput.h"
#include "SpellLanguageDialog.h"

#include <qce/SimpleTextDocument.h>
#include <qce/encoding/Encoding.h>
#include <qce/spell/CodeEditSpelling.h>

#include <QSet>
#include <QKeyEvent>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QTimer>
#include <QWindow>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>

#include <climits>
#include <memory>

namespace {
constexpr int kMaximumVisibleLines = 8;
constexpr int kRecentSpellLanguages = 6;
// Fewer words than this do not show the language reliably; the language found before stays.
constexpr int kLanguageWords = 3;
constexpr double kLanguageConfidence = 0.5;
}

MessageInput::MessageInput(QWidget *parent)
    : qce::CodeEditArea(parent)
    , document_(new qce::SimpleTextDocument(this))
    , spelling_(nullptr)
    , languageTimer_(new QTimer(this))
    , detectedLanguage_(QLocale::system().name().section('_', 0, 0))
{
    setDocument(document_);
    // Tab moves to the next control, as in other input fields.
    setTabCaptured(false);
    setWordWrap(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(this, &qce::CodeEditArea::textChanged, this, [this] {
        ++revision_;
        if (!recalling_) recalledUntouched_ = false;
        const QString text = toPlainText();
        if (text.isEmpty()) typed_ = false;
        if (!text.contains('\n')) typedLineBreak_ = false;
        fitHeight();
        updateEnterAction();
        if (spelling_ && spellLanguages_.isEmpty() && spelling_->isEnabled()) languageTimer_->start();
    });
    fitHeight();
    // Wavy underlines under misspelled words; the language follows the message until the user chooses one.
    spelling_ = new qce::spell::CodeEditSpelling(this, this);
    languageTimer_->setSingleShot(true);
    languageTimer_->setInterval(600);
    connect(languageTimer_, &QTimer::timeout, this, &MessageInput::detectLanguage);
    // A missing dictionary is reported once the owner can hear it, when it sets the languages.
    spelling_->setLanguages({detectedLanguage_});
}

bool MessageInput::spellCheckingEnabled() const
{
    return spelling_->isEnabled();
}

void MessageInput::setSpellCheckingEnabled(bool enabled)
{
    spelling_->setEnabled(enabled);
    if (enabled) applySpellLanguages();
}

void MessageInput::setSpellLanguages(const QStringList &dictionaries)
{
    spellLanguages_ = dictionaries;
    applySpellLanguages();
}

// The chosen ones become the most recent in the Spelling Language menu.
void MessageInput::chooseSpellLanguages(const QStringList &dictionaries)
{
    for (auto it = dictionaries.crbegin(); it != dictionaries.crend(); ++it) {
        recentSpellLanguages_.removeAll(*it);
        recentSpellLanguages_.prepend(*it);
    }
    recentSpellLanguages_ = recentSpellLanguages_.mid(0, kRecentSpellLanguages);
    setSpellLanguages(dictionaries);
    emit spellLanguagesChosen();
}

// The language of a few words is a guess; only a likely one replaces the language found before.
void MessageInput::detectLanguage()
{
    const QString text = toPlainText().simplified();
    if (text.count(' ') + 1 < kLanguageWords) return;
    const int request = ++languageRequest_;
    qce::encoding::detectLanguageAsync(text.toUtf8(), this, [this, request](const qce::encoding::Language &language) {
        if (request != languageRequest_ || language.code.isEmpty() || language.confidence < kLanguageConfidence
            || language.code == detectedLanguage_)
            return;
        detectedLanguage_ = language.code;
        applySpellLanguages();
    });
}

void MessageInput::applySpellLanguages()
{
    const QStringList languages = spellLanguages_.isEmpty() ? QStringList{detectedLanguage_} : spellLanguages_;
    spelling_->setLanguages(languages);
    if (spelling_->isReady() || !spelling_->isEnabled()) return;
    // Said once per language: which dictionary package would add it.
    const QString missing = languages.join(',');
    if (missing == missingDictionary_) return;
    missingDictionary_ = missing;
    const QString name = qce::encoding::languageName(languages.front());
    emit spellingDictionaryMissing(QString("No spelling dictionary for %1; install one with hunspell, e.g. "
                                           "sudo apt install %2")
                                       .arg(name.isEmpty() ? languages.front() : name,
                                            qce::spell::packageHint(languages.front())));
}

void MessageInput::addSpellingCorrections(QMenu &menu, const QPoint &position)
{
    if (!spelling_->isEnabled()) return;
    const auto word = spelling_->misspelledWordAt(cursorAt(position));
    if (!word) return;
    const QStringList suggestions = spelling_->suggestions(word->word, 8);
    if (suggestions.isEmpty()) menu.addAction("No suggestions")->setEnabled(false);
    for (const QString &suggestion : suggestions) {
        connect(menu.addAction(QString(suggestion).replace("&", "&&")), &QAction::triggered, this,
                [this, misspelling = *word, suggestion] { spelling_->replaceWord(misspelling, suggestion); });
    }
    menu.addSeparator();
    connect(menu.addAction("Add to Dictionary"), &QAction::triggered, this,
            [this, text = word->word] { spelling_->addToDictionary(text); });
    connect(menu.addAction("Ignore"), &QAction::triggered, this, [this, text = word->word] { spelling_->ignore(text); });
    menu.addSeparator();
}

// Automatic, the recent dictionaries and the ones in use, up to two at once, and all installed ones in a window.
void MessageInput::addSpellingLanguageMenu(QMenu &menu)
{
    QMenu *languageMenu = menu.addMenu("Spelling Language");
    languageMenu->setEnabled(spelling_->isEnabled());
    QHash<QString, QString> names;
    for (const auto &dictionary : qce::spell::installedDictionaries()) names.insert(dictionary.name, dictionary.displayName);
    const QString detected = qce::encoding::languageName(detectedLanguage_);
    QAction *automatic = languageMenu->addAction(detected.isEmpty() ? "Automatic" : QString("Automatic (%1)").arg(detected));
    automatic->setCheckable(true);
    automatic->setChecked(spellLanguages_.isEmpty());
    connect(automatic, &QAction::triggered, this, [this] { chooseSpellLanguages({}); });
    languageMenu->addSeparator();
    QStringList candidates = recentSpellLanguages_;
    for (const QString &name : spelling_->dictionaries())
        if (!candidates.contains(name)) candidates << name;
    for (const QString &name : candidates) {
        if (!names.contains(name)) continue;
        QAction *action = languageMenu->addAction(QString(names.value(name)).replace("&", "&&"));
        action->setCheckable(true);
        action->setChecked(spellLanguages_.contains(name));
        connect(action, &QAction::triggered, this, [this, name](bool checked) {
            QStringList chosen = spellLanguages_;
            chosen.removeAll(name);
            if (checked) chosen << name;
            while (chosen.size() > SpellLanguageDialog::MaxLanguages) chosen.removeFirst();
            chooseSpellLanguages(chosen);
        });
    }
    languageMenu->addSeparator();
    connect(languageMenu->addAction("More..."), &QAction::triggered, this, [this] {
        SpellLanguageDialog dialog(spellLanguages_.isEmpty() ? spelling_->dictionaries() : spellLanguages_, this);
        if (dialog.exec() == QDialog::Accepted) chooseSpellLanguages(dialog.chosenDictionaries());
    });
}

QString MessageInput::toPlainText() const
{
    return document_->toPlainText();
}

void MessageInput::setPlainText(const QString &text)
{
    document_->setText(text);
}

// Each message is recalled once, at the place of its latest use, so repeated messages do not alternate.
void MessageInput::setHistory(const QStringList &messages)
{
    QStringList unique;
    QSet<QString> seen;
    for (auto it = messages.crbegin(); it != messages.crend(); ++it) {
        const QString key = it->trimmed();
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        unique.prepend(*it);
    }
    if (unique == history_) return;
    const bool browsing = position_ < history_.size();
    history_ = unique;
    position_ = history_.size();
    if (browsing) replaceText(draft_);
    emit historyChanged();
}

namespace {
// A list shown as a popup closes like a combo box's: on Escape, on a click outside it and when the application
// stops being active. The whole application's clicks are watched while it is open, and such a click only closes
// it. On Wayland a click elsewhere in the window comes to the list itself, outside its rectangle; Qt's own
// check misses it, as it compares global positions, which Wayland does not give, and a scroll area passes
// clicks on its frame to no mouse handler.
class HistoryList : public QListWidget
{
public:
    explicit HistoryList(QWidget *parent) : QListWidget(parent) { qApp->installEventFilter(this); }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::ApplicationDeactivate) {
            close();
        } else if (isVisible() && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)) {
            bool inside = false;
            if (object == this || object == windowHandle())
                inside = rect().contains(static_cast<QMouseEvent *>(event)->position().toPoint());
            else if (auto *widget = qobject_cast<QWidget *>(object))
                inside = isAncestorOf(widget);
            if (!inside) {
                close();
                return true;
            }
        }
        return QListWidget::eventFilter(object, event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) close();
        else QListWidget::keyPressEvent(event);
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::WindowDeactivate) close();
        return QListWidget::event(event);
    }
};
}

void MessageInput::showHistoryList()
{
    if (history_.isEmpty()) return;
    // A parent makes it a popup of this window, also on Wayland.
    auto *list = new HistoryList(this);
    list->setWindowFlags(Qt::Popup);
    list->setAttribute(Qt::WA_DeleteOnClose);
    list->setObjectName("messageHistory");
    list->setFont(font());
    list->setUniformItemSizes(true);
    // Wide enough for the field's first line with the marker and the scroll bar beside it.
    const int width = this->width() + list->verticalScrollBar()->sizeHint().width()
                      + QFontMetrics(font()).horizontalAdvance(" …");
    // The newest message comes first. A row shows the message's beginning; the tooltip has all of it.
    for (auto it = history_.crbegin(); it != history_.crend(); ++it) {
        auto *item = new QListWidgetItem(beginning(*it), list);
        item->setToolTip(*it);
    }
    const auto rowOf = [this](qsizetype position) { return int(history_.size() - 1 - position); };
    // The message being recalled, or the newest, is selected.
    list->setCurrentRow(position_ < history_.size() ? rowOf(position_) : 0);
    const int rowHeight = list->sizeHintForRow(0);
    const int frame = 2 * list->frameWidth();
    int height = qMin(int(history_.size()), 12) * rowHeight + frame;

    // Below the field when it fits, as a combo box opens; otherwise on the side with more room, shorter if
    // even that is too small. On Wayland a window does not know where it is on the screen, and a popup that
    // reaches out of its window is placed oddly by some compositors, so there the list stays within the window.
    const QRect field(mapToGlobal(QPoint(0, 0)), size());
    QPoint position = field.bottomLeft() + QPoint(0, 1);
    QRect available;
    if (QGuiApplication::platformName().startsWith("wayland"))
        available = QRect(window()->mapToGlobal(QPoint(0, 0)), window()->size());
    else if (const QScreen *screen = this->screen())
        available = screen->availableGeometry();
    if (available.isValid()) {
        const int below = available.bottom() - field.bottom();
        const int above = field.top() - available.top();
        // Without room for one row on either side the list does not open.
        if (qMax(above, below) < rowHeight + frame) {
            delete list;
            return;
        }
        if (height > below && above > below) {
            height = qMin(height, above);
            position = QPoint(field.left(), field.top() - height);
        } else {
            height = qMin(height, below);
        }
        position.setX(qBound(available.left(), position.x(), qMax(available.left(), available.right() - width + 1)));
    }
    list->resize(width, height);
    list->move(position);
    const auto choose = [this, list, rowOf](QListWidgetItem *item) {
        const int row = list->row(item);
        list->close();
        recallAt(rowOf(row));
        setFocus();
    };
    connect(list, &QListWidget::itemActivated, this, choose);
    connect(list, &QListWidget::itemClicked, this, choose);
    list->show();
    list->scrollToItem(list->currentItem());
    list->setFocus();
}

void MessageInput::replaceText(const QString &text)
{
    selectAll();
    insertText(text);
}

void MessageInput::showSuggestion(const QString &text)
{
    replaceText(text);
    selectAll();
    typed_ = false;
    typedLineBreak_ = false;
    updateEnterAction();
    setFocus();
}

void MessageInput::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
        if (modifiers == Qt::ShiftModifier) {
            insertLineBreak();
            return;
        }
        if (modifiers == Qt::ControlModifier) {
            submit();
            return;
        }
        if (modifiers == Qt::NoModifier) {
            if (enterSends()) submit();
            else insertLineBreak();
            return;
        }
    }
    if ((event->modifiers() & ~Qt::KeypadModifier) == Qt::AltModifier && event->key() == Qt::Key_Down) {
        showHistoryList();
        return;
    }
    if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
        // A recalled message nobody has touched is skipped as a whole, not walked line by line.
        if (event->key() == Qt::Key_PageUp || (event->key() == Qt::Key_Up && (recalledUntouched_ || onFirstLine()))) {
            recall(-1);
            return;
        }
        if (event->key() == Qt::Key_PageDown || (event->key() == Qt::Key_Down && (recalledUntouched_ || onLastLine()))) {
            recall(1);
            return;
        }
    }
    if (event->matches(QKeySequence::Paste)) {
        pasteTrimmed();
        return;
    }
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Home
        || event->key() == Qt::Key_End)
        recalledUntouched_ = false;
    // Any change made from the keyboard counts as typing, except pasting.
    const int revision = revision_;
    qce::CodeEditArea::keyPressEvent(event);
    // Clearing the field by keyboard leaves nothing typed.
    if (revision_ != revision) typed_ = !toPlainText().isEmpty();
    updateEnterAction();
}

void MessageInput::setEnterPolicy(EnterPolicy policy)
{
    enterPolicy_ = policy;
    updateEnterAction();
}

void MessageInput::setShortMessageLength(int length)
{
    shortMessageLength_ = length;
    updateEnterAction();
}

void MessageInput::updateEnterAction()
{
    const bool sends = enterSends();
    if (sends == enterSendsNow_) return;
    enterSendsNow_ = sends;
    emit enterActionChanged(sends);
}

bool MessageInput::enterSends() const
{
    if (enterPolicy_ != EnterPolicy::Smart) return enterPolicy_ == EnterPolicy::Send;
    if (!typed_) return true;
    return !typedLineBreak_ && toPlainText().trimmed().size() <= shortMessageLength_;
}

void MessageInput::insertLineBreak()
{
    insertText("\n");
    typed_ = true;
    typedLineBreak_ = true;
    updateEnterAction();
}

void MessageInput::submit()
{
    position_ = history_.size();
    draft_.clear();
    emit submitted();
}

// Lines are the message's own lines; a long wrapped line is passed over as a whole.
bool MessageInput::onFirstLine() const
{
    return cursorPosition().line == 0;
}

bool MessageInput::onLastLine() const
{
    return cursorPosition().line == document_->lineCount() - 1;
}

void MessageInput::moveToEnd()
{
    setCursorPosition({document_->lineCount() - 1, INT_MAX});
}

// Scrolls the cursor into view and keeps the selection.
void MessageInput::ensureCursorVisible()
{
    const qce::TextCursor cursor = cursorPosition();
    const qce::TextCursor anchor = cursor == selectionEnd() ? selectionStart() : selectionEnd();
    setSelection(anchor, cursor);
}

// Going back puts the cursor at the end of the message and going forward at its start, so the arrow
// keys continue through its lines before reaching the next message.
void MessageInput::recall(int step)
{
    const qsizetype target = position_ + step;
    if (target < 0 || target > history_.size()) return;
    recallAt(target);
    if (step < 0) moveToEnd();
    else setCursorPosition({0, 0});
}

// The field shows whole character cells of the editor's grid; a line wraps at the last space that fits.
QString MessageInput::beginning(const QString &message) const
{
    const QString paragraph = message.section('\n', 0, 0);
    const int cell = viewportState().charWidth > 0 ? viewportState().charWidth
                                                   : QFontMetrics(font()).horizontalAdvance('M');
    // The editor leaves a few pixels before the text.
    const int columns = qMax(1, (viewport()->width() - 4) / qMax(1, cell));
    int length = int(paragraph.size());
    if (length > columns) {
        const int space = int(paragraph.lastIndexOf(' ', columns));
        length = space > 0 ? space : columns;
    }
    QString first = paragraph.left(length);
    while (!first.isEmpty() && first.back().isSpace()) first.chop(1);
    const bool more = length < paragraph.size() || !message.mid(paragraph.size()).trimmed().isEmpty();
    return more ? first + " …" : first;
}

// Shows the message at this place of the history, or the draft past its end, as recalled and untouched.
void MessageInput::recallAt(qsizetype target)
{
    if (target < 0 || target > history_.size()) return;
    if (position_ == history_.size()) draft_ = toPlainText();
    position_ = target;
    recalling_ = true;
    replaceText(position_ == history_.size() ? draft_ : history_.at(position_));
    recalling_ = false;
    recalledUntouched_ = position_ < history_.size();
    typed_ = false;
    updateEnterAction();
    moveToEnd();
}

// Line breaks, spaces and tabs around pasted text are dropped: a trailing line break would make Enter
// start a new line instead of sending.
// Pasted text that replaces the whole field counts as unchanged, so Enter sends it whatever its length.
void MessageInput::pasteTrimmed()
{
    const QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) return;
    const qce::TextCursor end{document_->lineCount() - 1, int(document_->lineAt(document_->lineCount() - 1).size())};
    const bool replacesAll = toPlainText().isEmpty()
        || (selectionStart() == qce::TextCursor{0, 0} && selectionEnd() == end);
    insertText(text);
    if (replacesAll) {
        typed_ = false;
        typedLineBreak_ = false;
    }
    updateEnterAction();
}

// Corrections of the misspelled word under the mouse come first, then the editing actions and the spelling
// language.
void MessageInput::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    // A click's position comes in the viewport's coordinates, the menu key's in this widget's.
    addSpellingCorrections(menu, event->reason() == QContextMenuEvent::Keyboard ? viewport()->mapFrom(this, event->pos())
                                                                                 : event->pos());
    const std::unique_ptr<QMenu> standard(createStandardContextMenu(this));
    // Paste trims the text as Ctrl+V does here.
    for (QAction *action : standard->actions()) {
        if (action->shortcut() == QKeySequence(QKeySequence::Paste)) {
            disconnect(action, &QAction::triggered, nullptr, nullptr);
            connect(action, &QAction::triggered, this, &MessageInput::pasteTrimmed);
        }
    }
    menu.addActions(standard->actions());
    menu.addSeparator();
    addSpellingLanguageMenu(menu);
    menu.exec(event->globalPos());
}

// The field grows after its text has changed, when the scrolling has already been decided for the smaller
// height; the cursor is brought into view again at the new size. A new width wraps the text anew.
void MessageInput::resizeEvent(QResizeEvent *event)
{
    qce::CodeEditArea::resizeEvent(event);
    fitHeight();
    if (hasFocus()) ensureCursorVisible();
}

void MessageInput::changeEvent(QEvent *event)
{
    qce::CodeEditArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) fitHeight();
}

void MessageInput::mousePressEvent(QMouseEvent *event)
{
    recalledUntouched_ = false;
    qce::CodeEditArea::mousePressEvent(event);
}

void MessageInput::fitHeight()
{
    const int lines = qBound(1, visualRowCount(), kMaximumVisibleLines);
    const int lineHeight = viewportState().lineHeight > 0 ? viewportState().lineHeight : fontMetrics().lineSpacing();
    // The margins include the frame. With any spare pixels a partly shown row would count as visible, and the
    // editor would not scroll to the cursor in it.
    const QMargins margins = contentsMargins();
    setFixedHeight(lines * lineHeight + margins.top() + margins.bottom());
}
