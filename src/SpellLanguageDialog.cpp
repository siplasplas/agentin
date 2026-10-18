#include "SpellLanguageDialog.h"

#include <qce/spell/SpellChecker.h>

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

SpellLanguageDialog::SpellLanguageDialog(const QStringList &chosen, QWidget *parent)
    : QDialog(parent)
    , list_(new QListWidget(this))
{
    setWindowTitle("Spelling Languages");
    resize(420, 460);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel("Check one or two languages; a word is right in either (e.g. English words in a "
                             "Polish message).", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    list_->setObjectName("spellLanguages");
    const auto dictionaries = qce::spell::installedDictionaries();
    for (const auto &dictionary : dictionaries) {
        auto *item = new QListWidgetItem(QString("%1 (%2)").arg(dictionary.displayName, dictionary.name), list_);
        item->setData(Qt::UserRole, dictionary.name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(chosen.contains(dictionary.name) ? Qt::Checked : Qt::Unchecked);
        if (chosen.contains(dictionary.name)) order_ << dictionary.name;
    }
    if (dictionaries.isEmpty()) list_->addItem("No hunspell dictionaries are installed.");
    layout->addWidget(list_, 1);

    auto *hint = new QLabel(QString("Other languages are installed as hunspell dictionaries, e.g. "
                                    "<tt>sudo apt install %1</tt> for German, then open this window again.")
                                .arg(qce::spell::packageHint("de")), this);
    hint->setWordWrap(true);
    hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(hint);

    // At most two: checking a third unchecks the one checked first.
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const QString name = item->data(Qt::UserRole).toString();
        if (name.isEmpty()) return;
        order_.removeAll(name);
        if (item->checkState() == Qt::Checked) order_ << name;
        while (order_.size() > MaxLanguages) {
            const QString dropped = order_.takeFirst();
            for (int i = 0; i < list_->count(); ++i) {
                if (list_->item(i)->data(Qt::UserRole).toString() == dropped) {
                    const QSignalBlocker block(list_);
                    list_->item(i)->setCheckState(Qt::Unchecked);
                }
            }
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
