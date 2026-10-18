#pragma once

#include <qce/CodeEditArea.h>

#include <QStringList>

class QMenu;
class QTimer;

namespace qce {
class SimpleTextDocument;
namespace spell {
class CodeEditSpelling;
}
}

// Message field on qcodeedit's editor that grows with its text. Shift+Enter always starts a new line, Ctrl+Enter
// always sends, and Enter follows the EnterPolicy. Up and Down move between lines and, on the first or last
// line, recall the previous or next message of the conversation; Page Up and Page Down always recall
// messages. Past the newest message the text typed before browsing comes back. Alt+Down shows the messages in
// a list, as a combo box does.
class MessageInput : public qce::CodeEditArea
{
    Q_OBJECT

public:
    enum class EnterPolicy {
        // Enter sends a recalled message or pasted text left unchanged, and a typed message of one
        // line up to shortMessageLength() characters. A longer typed message, or one where the user
        // typed a line break, gets a new line; Ctrl+Enter sends it.
        Smart,
        Send,
        NewLine,
    };

    explicit MessageInput(QWidget *parent = nullptr);
    ~MessageInput() override;

    QString toPlainText() const;
    // Replaces the text and the undo history, as a new message does.
    void setPlainText(const QString &text);
    void clear() { setPlainText({}); }

    EnterPolicy enterPolicy() const { return enterPolicy_; }
    void setEnterPolicy(EnterPolicy policy);
    // 0 means that Enter never sends typed text in the Smart policy.
    int shortMessageLength() const { return shortMessageLength_; }
    void setShortMessageLength(int length);
    // What Enter does for the current text; Shift+Enter and Ctrl+Enter do the other.
    bool enterSends() const;

    // Earlier messages of the conversation, oldest first. Resets browsing.
    void setHistory(const QStringList &messages);
    // Replaces the text as one edit that Ctrl+Z can undo, unlike setPlainText() and clear().
    void replaceText(const QString &text);
    // Shows a suggested message selected, so that typing replaces it and Enter sends it.
    void showSuggestion(const QString &text);
    bool hasHistory() const { return !history_.isEmpty(); }
    // Lists the earlier messages below the field, newest first, as a combo box lists its items; the chosen one is
    // recalled as Page Up would recall it.
    void showHistoryList();

    // Misspelled words get a wavy underline; the context menu offers corrections. On by default.
    bool spellCheckingEnabled() const;
    void setSpellCheckingEnabled(bool enabled);
    // The dictionaries chosen in the context menu (e.g. "en_US", "pl_PL"); empty means the language detected
    // from the message.
    QStringList spellLanguages() const { return spellLanguages_; }
    void setSpellLanguages(const QStringList &dictionaries);
    // Recently chosen dictionaries, newest first, offered in the context menu.
    QStringList recentSpellLanguages() const { return recentSpellLanguages_; }
    void setRecentSpellLanguages(const QStringList &dictionaries) { recentSpellLanguages_ = dictionaries; }
    // The language detected from the message, a cpg code such as "pl"; at first the desktop's.
    QString detectedLanguage() const { return detectedLanguage_; }

signals:
    void submitted();
    void enterActionChanged(bool sends);
    void historyChanged();
    // The user chose spelling languages in the context menu.
    void spellLanguagesChosen();
    // No dictionary is installed for the language; the text says which package adds one.
    void spellingDictionaryMissing(const QString &text);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    bool onFirstLine() const;
    bool onLastLine() const;
    void moveToEnd();
    void ensureCursorVisible();
    void recall(int step);
    void recallAt(qsizetype position);
    // The beginning of a message as the field would show it: up to its first line break or the end of its
    // first wrapped line at the field's width, with " …" when more follows.
    QString beginning(const QString &message) const;
    void updateEnterAction();
    void insertLineBreak();
    void pasteTrimmed();
    void submit();
    void fitHeight();
    void detectLanguage();
    void applySpellLanguages();
    void chooseSpellLanguages(const QStringList &dictionaries);
    // `position` in the viewport's coordinates.
    void addSpellingCorrections(QMenu &menu, const QPoint &position);
    void addSpellingLanguageMenu(QMenu &menu);

    qce::SimpleTextDocument *document_;
    qce::spell::CodeEditSpelling *spelling_;
    // Detects the message's language a moment after typing stops.
    QTimer *languageTimer_;
    int languageRequest_ = 0;
    // The language detected from the message (a cpg code such as "pl"), kept while the text is too short to tell.
    QString detectedLanguage_;
    QStringList spellLanguages_;
    QStringList recentSpellLanguages_;
    // The languages last reported without a dictionary, so that each is reported once.
    QString missingDictionary_;
    QStringList history_;
    // history_.size() while editing the draft rather than a recalled message.
    qsizetype position_ = 0;
    QString draft_;
    EnterPolicy enterPolicy_ = EnterPolicy::Smart;
    // The user changed the current text with the keyboard; pasting and recalling do not count.
    bool typed_ = false;
    bool typedLineBreak_ = false;
    // A message recalled from the history that has been neither edited nor entered with the cursor.
    bool recalledUntouched_ = false;
    bool recalling_ = false;
    // Counts changes of the text, to tell whether a key changed it.
    int revision_ = 0;
    int shortMessageLength_ = 60;
    bool enterSendsNow_ = true;
};
