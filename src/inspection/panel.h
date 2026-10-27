#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QWidget>
#include <array>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QGroupBox;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Native dock content. Requests and updates stay on the GUI thread, and there is
// no polling. The owner supplies a synchronous in-process protocol dispatcher.
class InspectionPanel final : public QWidget {
public:
    explicit InspectionPanel(std::function<QJsonObject(const QJsonObject&)> send,
                             QWidget* parent=nullptr);
    void setState(const QString& id, const QJsonObject& state);
    void refresh();
    bool setToolMode(const QString& mode);
    bool commitPendingCuts();

    std::function<void(QString)> reportError;

private:
    struct CutControls {
        QString axis;
        QCheckBox* enabled=nullptr;
        QSlider* slider=nullptr;
        QDoubleSpinBox* position=nullptr;
        QComboBox* keep=nullptr;
        double low=-1, high=1;
    };

    QJsonObject request(const QString& method, const QString& action,
                        QJsonObject fields={}, bool applyState=true);
    void applyResponse(const QJsonObject& response);
    void showError(const QString& text);
    void updateSelection();
    void updateVisibility();
    void updateCuts();
    void updateCommentControls();
    void updateNoteSelection();
    void updatePartsSummary();
    void updateNotesSummary();
    void filterParts();
    void beginParts();
    void loadPartsPage(quint64 generation);
    void beginNotes();
    void loadNotesPage(quint64 generation);
    void queueCut(int index);
    bool flushCuts();
    void resetCuts();
    void addComment();
    QJsonObject selectedIdentity() const;
    QJsonObject selectedNote() const;
    QJsonObject expectedDocument() const;

    std::function<QJsonObject(const QJsonObject&)> send;
    QString id;
    QJsonObject state;
    bool loaded=false, stateKnown=false;
    bool partsRequested=false, notesRequested=false;
    bool partsLoading=false, notesLoading=false;
    bool partsMore=false, notesMore=false;
    int partsOffset=0, partsTotal=0, notesOffset=0, notesTotal=0;
    quint64 partsGeneration=0, notesGeneration=0;
    QJsonValue inventoryRevision, notesRevision;
    QString inventoryId, notesId, notesDigest, notesPath;
    QString selectedNoteId;
    QSet<QString> hidden;
    QHash<QString, QJsonObject> parts, notes;
    QHash<QString, QTreeWidgetItem*> partItems, noteItems;
    std::array<CutControls, 3> cuts;
    QSet<int> pendingCuts;

    QLabel *availability, *feedback, *selectionName, *selectionHint;
    QLabel *partsSummary, *notesSummary, *commentTarget, *commentSize;
    QLineEdit *reviewPath, *search;
    QToolButton *copyReview, *chooseReview;
    QPushButton *returnButton, *copyIdentity, *hidePart, *showPart, *isolatePart;
    QPushButton *undoVisibility, *restoreVisibility, *resetCutButton;
    QPushButton *moreParts, *moreNotes, *addButton, *restoreNote, *resolveNote, *deleteNote;
    QTreeWidget *partTree, *noteTree;
    QPlainTextEdit *comment, *noteText;
    QComboBox *commentScope, *commentStatus;
    QTimer* cutTimer;
    QGroupBox *partsBox, *cutsBox;
    QString toolMode = "parts";
};
