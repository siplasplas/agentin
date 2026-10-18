#pragma once

#include <QDialog>
#include <QStringList>

class QListWidget;

// Chooses one or two spelling dictionaries among all hunspell has installed (e.g. English + Polish), with a hint
// how to install another language.
class SpellLanguageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SpellLanguageDialog(const QStringList &chosen, QWidget *parent = nullptr);

    // The checked dictionaries (names such as "pl_PL"), at most two, oldest first.
    QStringList chosenDictionaries() const { return order_; }

    static constexpr int MaxLanguages = 2;

private:
    QListWidget *list_;
    QStringList order_;
};
