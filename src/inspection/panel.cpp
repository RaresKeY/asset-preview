#include "panel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int PageSize=100;
constexpr int RowLimit=4096;
constexpr int CutSteps=1000;
constexpr int NoteBytes=8192;

class CutSlider final : public QSlider {
public:
    explicit CutSlider(QWidget* parent) : QSlider(Qt::Horizontal,parent) {}
protected:
    bool event(QEvent* event) override {
        if (event->type()==QEvent::ShortcutOverride) {
            const auto* key=static_cast<QKeyEvent*>(event);
            if (!(key->modifiers() & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))) {
                switch (key->key()) {
                case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
                case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
                    event->accept(); return true;
                default: break;
                }
            }
        }
        return QSlider::event(event);
    }
};

QString partName(const QJsonObject& part) {
    for (const auto* field:{"name", "source_manufactured_label", "source_label", "exported_name", "semantic_id"}) {
        const auto name=part[field].toString();
        if (!name.isEmpty()) return name;
    }
    return "Unnamed part";
}

QString binding(const QJsonObject& note) {
    if (note["binding"].isString()) return note["binding"].toString();
    if (note["binding"].isObject()) {
        const auto value=note["binding"].toObject();
        return value["state"].toString(value["status"].toString("stale"));
    }
    return note["binding_state"].toString("stale");
}

QString statusName(const QString& status) {
    if (status=="in_progress") return "In progress";
    if (status=="resolved") return "Resolved";
    if (status=="obsolete") return "Obsolete";
    return "Open";
}

void renderNote(QTreeWidgetItem* item, const QJsonObject& note) {
    auto preview=note["text"].toString().simplified();
    if (preview.size()>100) preview=preview.left(97)+"...";
    const auto bound=binding(note);
    item->setText(0,(bound=="exact"?QString():"["+bound+"] ")+statusName(note["status"].toString())+": "+preview);
    item->setToolTip(0,"Status: "+statusName(note["status"].toString())+"\nBinding: "+bound+"\nID: "+note["id"].toString()+"\n\n"+note["text"].toString());
}

QString partTooltip(const QJsonObject& part, bool hidden) {
    QStringList lines{partName(part)};
    const auto exported=part["exported_name"].toString();
    if (!exported.isEmpty() && exported!=lines.front()) lines.append("Exported name: "+exported);
    for (const auto* field:{"path", "semantic_id", "id"}) {
        const auto value=part[field].toString();
        if (!value.isEmpty()) lines.append(QString::fromLatin1(field)+": "+value);
    }
    if (hidden) lines.append("Hidden from the model view");
    return lines.join('\n');
}

QLabel* label(QWidget* parent, const QString& text={}) {
    auto* result=new QLabel(text,parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setMinimumWidth(0);
    result->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    return result;
}

QPushButton* button(QWidget* parent, const QString& text, const QString& name,
                    const QString& tooltip={}) {
    auto* result=new QPushButton(text,parent);
    result->setObjectName(name);
    result->setAccessibleName(tooltip.isEmpty()?text:tooltip);
    result->setToolTip(tooltip.isEmpty()?text:tooltip);
    result->setAutoDefault(false);
    result->setMinimumWidth(0);
    return result;
}

QTreeWidget* tree(QWidget* parent, const QString& name, const QString& accessible) {
    auto* result=new QTreeWidget(parent);
    result->setObjectName(name);
    result->setAccessibleName(accessible);
    result->setColumnCount(1);
    result->setHeaderHidden(true);
    result->setRootIsDecorated(false);
    result->setUniformRowHeights(true);
    result->setSelectionMode(QAbstractItemView::SingleSelection);
    result->setSelectionBehavior(QAbstractItemView::SelectRows);
    result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->setMinimumSize(0,110);
    result->setMaximumHeight(220);
    result->header()->setStretchLastSection(true);
    return result;
}
}

InspectionPanel::InspectionPanel(std::function<QJsonObject(const QJsonObject&)> dispatcher, QWidget* parent)
    : QWidget(parent), send(std::move(dispatcher)) {
    setObjectName("inspectionPanel");
    setAccessibleName("3D inspection");
    setMinimumWidth(280);
    auto* outer=new QVBoxLayout(this);
    outer->setContentsMargins(0,0,0,0);
    auto* scroll=new QScrollArea(this);
    scroll->setObjectName("inspectionScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto* content=new QWidget(scroll);
    auto* layout=new QVBoxLayout(content);
    layout->setContentsMargins(8,8,8,8);
    layout->setSpacing(8);
    scroll->setWidget(content);

    returnButton=button(content,"Return to Preview","inspectionReturn","Return to the previous preview layout");
    layout->addWidget(returnButton);
    connect(returnButton,&QPushButton::clicked,this,[this] {
        if (commitPendingCuts()) request("inspect","close");
    });
    availability=label(content,"No model loaded.");
    availability->setAccessibleName("Inspection availability");
    layout->addWidget(availability);
    feedback=label(content);
    feedback->setObjectName("inspectionFeedback");
    feedback->setAccessibleName("Inspection feedback");
    feedback->setTextInteractionFlags(Qt::TextSelectableByMouse);
    feedback->hide();
    layout->addWidget(feedback);

    auto* reviewBox=new QGroupBox("Review document",content);
    auto* reviewLayout=new QGridLayout(reviewBox);
    reviewLayout->setContentsMargins(8,8,8,8);
    reviewLayout->setSpacing(4);
    reviewPath=new QLineEdit(reviewBox);
    reviewPath->setObjectName("inspectionReviewPath");
    reviewPath->setReadOnly(true);
    reviewPath->setMinimumWidth(0);
    reviewPath->setAccessibleName("Review document path");
    reviewPath->setPlaceholderText("Review path unavailable");
    auto tool=[this,reviewBox](const QString& icon, QStyle::StandardPixmap fallback, const QString& name) {
        auto* result=new QToolButton(reviewBox);
        result->setIcon(QIcon::fromTheme(icon,style()->standardIcon(fallback)));
        result->setAccessibleName(name);
        result->setToolTip(name);
        result->setFocusPolicy(Qt::StrongFocus);
        return result;
    };
    copyReview=tool("edit-copy",QStyle::SP_FileIcon,"Copy review document path");
    copyReview->setObjectName("inspectionCopyReview");
    chooseReview=tool("document-save-as",QStyle::SP_DialogSaveButton,"Choose an existing or new review document");
    chooseReview->setObjectName("inspectionChooseReview");
    reviewLayout->addWidget(reviewPath,0,0);
    reviewLayout->addWidget(copyReview,0,1);
    reviewLayout->addWidget(chooseReview,0,2);
    reviewLayout->setColumnStretch(0,1);
    layout->addWidget(reviewBox);
    connect(copyReview,&QToolButton::clicked,this,[this] { QApplication::clipboard()->setText(reviewPath->text()); });
    connect(chooseReview,&QToolButton::clicked,this,[this] {
        const auto path=QFileDialog::getSaveFileName(this,"Choose Review Document",reviewPath->text(),
            "JSON documents (*.json);;All files (*)",nullptr,QFileDialog::DontConfirmOverwrite);
        if (!path.isEmpty()) request("inspect","open",{{"review",QFileInfo(path).absoluteFilePath()}});
    });

    partsBox=new QGroupBox("Parts",content);
    partsBox->setObjectName("inspectionPartsGroup");
    auto* partsLayout=new QVBoxLayout(partsBox);
    partsLayout->setContentsMargins(8,8,8,8);
    partsLayout->setSpacing(4);
    search=new QLineEdit(partsBox);
    search->setObjectName("inspectionPartSearch");
    search->setAccessibleName("Search model parts");
    search->setPlaceholderText("Search parts");
    search->setClearButtonEnabled(true);
    partsLayout->addWidget(search);
    partTree=tree(partsBox,"inspectionParts","Model parts, use arrow keys to select a part");
    partsLayout->addWidget(partTree);
    partsSummary=label(partsBox);
    partsSummary->setAccessibleName("Part inventory summary");
    partsLayout->addWidget(partsSummary);
    moreParts=button(partsBox,"Load more parts","inspectionMoreParts");
    moreParts->hide();
    partsLayout->addWidget(moreParts);
    selectionName=label(partsBox,"No part selected");
    selectionName->setObjectName("inspectionSelectionName");
    selectionName->setAccessibleName("Selected component name");
    auto selectionFont=selectionName->font(); selectionFont.setBold(true); selectionName->setFont(selectionFont);
    selectionName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    partsLayout->addWidget(selectionName);
    selectionHint=label(partsBox,"Select a part in the tree or click its surface.");
    selectionHint->setAccessibleName("Selected component details");
    partsLayout->addWidget(selectionHint);
    copyIdentity=button(partsBox,"Copy identity","inspectionCopyIdentity","Copy the exact selected component identity as JSON");
    partsLayout->addWidget(copyIdentity);
    auto* partActions=new QGridLayout;
    partActions->setSpacing(4);
    hidePart=button(partsBox,"Hide","inspectionHide","Hide the selected component");
    showPart=button(partsBox,"Show","inspectionShow","Show the selected hidden component");
    isolatePart=button(partsBox,"Isolate","inspectionIsolate","Show only the selected component");
    undoVisibility=button(partsBox,"Undo","inspectionUndoVisibility","Undo the last hide or isolate operation");
    restoreVisibility=button(partsBox,"Restore all","inspectionRestoreVisibility","Restore visibility of all components");
    partActions->addWidget(hidePart,0,0); partActions->addWidget(showPart,0,1);
    partActions->addWidget(isolatePart,1,0); partActions->addWidget(undoVisibility,1,1);
    partActions->addWidget(restoreVisibility,2,0,1,2);
    partsLayout->addLayout(partActions);
    layout->addWidget(partsBox);
    connect(search,&QLineEdit::textChanged,this,[this] { filterParts(); });
    connect(partTree,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item) {
        if (!loaded || !item) return;
        const auto target=item->data(0,Qt::UserRole).toString();
        if (!target.isEmpty() && target!=state["selected"].toString()) request("inspect","select",{{"target",target}});
    });
    connect(moreParts,&QPushButton::clicked,this,[this] {
        if (!partsLoading && loaded) { partsLoading=true; loadPartsPage(partsGeneration); }
    });
    connect(copyIdentity,&QPushButton::clicked,this,[this] {
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(selectedIdentity()).toJson(QJsonDocument::Indented)));
    });
    for (const auto& [widget,action]:{std::pair{hidePart,"hide"},{showPart,"show"},{isolatePart,"isolate"}}) {
        connect(widget,&QPushButton::clicked,this,[this,action] { request("inspect",action,{{"target",state["selected"].toString()}}); });
    }
    connect(undoVisibility,&QPushButton::clicked,this,[this] { request("inspect","restore",{{"last",true}}); });
    connect(restoreVisibility,&QPushButton::clicked,this,[this] { request("inspect","restore",{{"all",true}}); });

    cutsBox=new QGroupBox("Planar cuts",content);
    cutsBox->setObjectName("inspectionCutsGroup");
    auto* cutsLayout=new QGridLayout(cutsBox);
    cutsLayout->setContentsMargins(8,8,8,8);
    cutsLayout->setHorizontalSpacing(4);
    cutsLayout->setVerticalSpacing(3);
    cutTimer=new QTimer(this);
    cutTimer->setSingleShot(true);
    cutTimer->setInterval(16);
    connect(cutTimer,&QTimer::timeout,this,[this] { flushCuts(); });
    for (int index=0; index<3; ++index) {
        auto& cut=cuts[index];
        cut.axis=QString(QChar('x'+index));
        const auto axis=cut.axis.toUpper();
        cut.enabled=new QCheckBox(axis+" cut",cutsBox);
        cut.enabled->setObjectName("inspectionCut"+axis+"Enabled");
        cut.enabled->setAccessibleName("Enable "+axis+" cut");
        cut.enabled->setToolTip("Clip along model "+axis+" in its exported coordinate frame");
        cut.position=new QDoubleSpinBox(cutsBox);
        cut.position->setObjectName("inspectionCut"+axis+"Position");
        cut.position->setAccessibleName(axis+" cut position in model units");
        cut.position->setDecimals(6);
        cut.position->setRange(-1,1);
        cut.position->setKeyboardTracking(false);
        cut.position->setMinimumWidth(0);
        cut.position->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
        cut.keep=new QComboBox(cutsBox);
        cut.keep->setObjectName("inspectionCut"+axis+"Keep");
        cut.keep->addItem("Keep +"+axis,"positive");
        cut.keep->addItem("Keep -"+axis,"negative");
        cut.keep->setAccessibleName(axis+" cut kept side");
        cut.keep->setToolTip("Keep the positive or negative half-space of this plane");
        cut.keep->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        cut.keep->setMinimumContentsLength(6);
        cut.slider=new CutSlider(cutsBox);
        cut.slider->setObjectName("inspectionCut"+axis+"Slider");
        cut.slider->setAccessibleName(axis+" cut position within loaded model bounds");
        cut.slider->setRange(0,CutSteps);
        cut.slider->setValue(CutSteps/2);
        cut.slider->setSingleStep(1);
        cut.slider->setPageStep(50);
        cut.slider->setMinimumWidth(0);
        cut.slider->setMinimumHeight(24);
        cutsLayout->addWidget(cut.enabled,index*2,0);
        cutsLayout->addWidget(cut.position,index*2,1);
        cutsLayout->addWidget(cut.keep,index*2,2);
        cutsLayout->addWidget(cut.slider,index*2+1,0,1,3);
        connect(cut.enabled,&QCheckBox::toggled,this,[this,index] { queueCut(index); });
        connect(cut.keep,&QComboBox::currentIndexChanged,this,[this,index] { queueCut(index); });
        connect(cut.position,&QDoubleSpinBox::valueChanged,this,[this,index](double position) {
            auto& current=cuts[index];
            const QSignalBlocker blocker(current.slider);
            current.slider->setValue(qRound(std::clamp((position-current.low)/(current.high-current.low),0.0,1.0)*CutSteps));
            queueCut(index);
        });
        connect(cut.slider,&QSlider::valueChanged,this,[this,index](int position) {
            auto& current=cuts[index];
            const QSignalBlocker positionBlocker(current.position), enableBlocker(current.enabled);
            current.position->setValue(current.low+(current.high-current.low)*position/CutSteps);
            current.enabled->setChecked(true);
            queueCut(index);
        });
        connect(cut.slider,&QSlider::sliderReleased,this,[this] {
            if (cutTimer->isActive()) { cutTimer->stop(); flushCuts(); }
        });
    }
    cutsLayout->setColumnStretch(1,1);
    auto* cutHint=label(cutsBox,"Drag a slider to enable and move its plane. Positions use model units; enabled cuts keep the intersection.");
    cutsLayout->addWidget(cutHint,6,0,1,3);
    resetCutButton=button(cutsBox,"Reset cuts","inspectionResetCuts","Disable all cuts and reset their positions");
    cutsLayout->addWidget(resetCutButton,7,0,1,3);
    connect(resetCutButton,&QPushButton::clicked,this,[this] { resetCuts(); });
    layout->insertWidget(layout->indexOf(partsBox),cutsBox);

    auto* commentsBox=new QGroupBox("Comments",content);
    auto* commentsLayout=new QVBoxLayout(commentsBox);
    commentsLayout->setContentsMargins(8,8,8,8);
    commentsLayout->setSpacing(4);
    noteTree=tree(commentsBox,"inspectionNotes","Saved review comments");
    commentsLayout->addWidget(noteTree);
    notesSummary=label(commentsBox);
    notesSummary->setAccessibleName("Review comments summary");
    commentsLayout->addWidget(notesSummary);
    moreNotes=button(commentsBox,"Load more comments","inspectionMoreNotes");
    moreNotes->hide();
    commentsLayout->addWidget(moreNotes);
    noteText=new QPlainTextEdit(commentsBox);
    noteText->setObjectName("inspectionNoteText");
    noteText->setAccessibleName("Selected review comment text");
    noteText->setReadOnly(true);
    noteText->setMinimumWidth(0);
    noteText->setFixedHeight(76);
    noteText->hide();
    commentsLayout->addWidget(noteText);
    auto* noteActions=new QGridLayout;
    noteActions->setSpacing(4);
    restoreNote=button(commentsBox,"Restore view","inspectionRestoreNote","Restore the selected comment's saved view and selection");
    resolveNote=button(commentsBox,"Resolve","inspectionResolveNote","Resolve the selected comment");
    deleteNote=button(commentsBox,"Delete","inspectionDeleteNote","Delete the selected comment");
    noteActions->addWidget(restoreNote,0,0,1,2);
    noteActions->addWidget(resolveNote,1,0); noteActions->addWidget(deleteNote,1,1);
    commentsLayout->addLayout(noteActions);
    auto* newLabel=label(commentsBox,"New comment");
    auto newFont=newLabel->font(); newFont.setBold(true); newLabel->setFont(newFont);
    commentsLayout->addWidget(newLabel);
    comment=new QPlainTextEdit(commentsBox);
    comment->setObjectName("inspectionCommentDraft");
    comment->setAccessibleName("New review comment text");
    comment->setPlaceholderText("Describe the issue or requested change.");
    comment->setMinimumWidth(0);
    comment->setFixedHeight(88);
    commentsLayout->addWidget(comment);
    auto* commentOptions=new QGridLayout;
    commentOptions->setSpacing(4);
    commentScope=new QComboBox(commentsBox);
    commentScope->setObjectName("inspectionCommentScope");
    commentScope->setAccessibleName("New comment target");
    commentScope->setToolTip("Current part or pin uses the selected component and surface point. Whole model creates a comment without a component target.");
    commentScope->addItem("Current part or pin","selection");
    commentScope->addItem("Whole model","model");
    commentScope->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    commentScope->setMinimumContentsLength(10);
    commentStatus=new QComboBox(commentsBox);
    commentStatus->setObjectName("inspectionCommentStatus");
    commentStatus->setAccessibleName("New comment status");
    commentStatus->addItem("Open","open");
    commentStatus->addItem("In progress","in_progress");
    commentStatus->addItem("Resolved","resolved");
    commentStatus->addItem("Obsolete","obsolete");
    commentOptions->addWidget(commentScope,0,0); commentOptions->addWidget(commentStatus,0,1);
    commentOptions->setColumnStretch(0,1);
    commentsLayout->addLayout(commentOptions);
    commentTarget=label(commentsBox,"Click the model to select a part. A model surface click can place a pin.");
    commentTarget->setObjectName("inspectionCommentTarget");
    commentTarget->setAccessibleName("New comment target details");
    commentsLayout->addWidget(commentTarget);
    commentSize=label(commentsBox);
    commentSize->setAccessibleName("New comment text size");
    commentsLayout->addWidget(commentSize);
    addButton=button(commentsBox,"Add comment","inspectionAddComment","Save a comment with the current view");
    commentsLayout->addWidget(addButton);
    layout->addWidget(commentsBox);
    layout->addStretch(1);
    connect(noteTree,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item) {
        selectedNoteId=item?item->data(0,Qt::UserRole).toString():QString();
        updateNoteSelection();
    });
    connect(moreNotes,&QPushButton::clicked,this,[this] {
        if (!notesLoading) { notesLoading=true; loadNotesPage(notesGeneration); }
    });
    connect(restoreNote,&QPushButton::clicked,this,[this] {
        cutTimer->stop(); pendingCuts.clear();
        updateCuts();
        request("annotations","restore",{{"note_id",selectedNoteId}});
    });
    connect(resolveNote,&QPushButton::clicked,this,[this] {
        const auto note=selectedNote();
        const auto status=note["status"].toString();
        auto fields=expectedDocument();
        fields["note_id"]=selectedNoteId;
        fields["note"]=QJsonObject{{"status",status=="resolved" || status=="obsolete"?"open":"resolved"}};
        request("annotations","update",fields);
    });
    connect(deleteNote,&QPushButton::clicked,this,[this] {
        auto fields=expectedDocument(); fields["note_id"]=selectedNoteId;
        request("annotations","remove",fields);
    });
    connect(comment,&QPlainTextEdit::textChanged,this,[this] { updateCommentControls(); });
    connect(commentScope,&QComboBox::currentIndexChanged,this,[this] { updateCommentControls(); });
    connect(addButton,&QPushButton::clicked,this,[this] { addComment(); });
    setState({},{});
}

QJsonObject InspectionPanel::request(const QString& method, const QString& action, QJsonObject fields, bool applyState) {
    if (id.isEmpty() || !send) return {{"ok",false},{"error","No inspected model is selected"}};
    fields["method"]=method; fields["id"]=id; fields["action"]=action;
    const auto result=send(fields);
    if (!result["ok"].toBool()) {
        showError(result["error"].toString("Inspection request failed"));
    } else {
        if (applyState) { feedback->clear(); feedback->hide(); applyResponse(result); }
    }
    return result;
}

void InspectionPanel::applyResponse(const QJsonObject& response) {
    QJsonObject next=response;
    if (response["state"].isObject()) next=response["state"].toObject();
    else if (response["inspection"].isObject()) next=response["inspection"].toObject();
    auto merged=state;
    for (auto item=next.begin(); item!=next.end(); ++item) {
        if (item.key()!="ok") merged[item.key()]=item.value();
    }
    setState(id,merged);
}

void InspectionPanel::showError(const QString& text) {
    feedback->setText(text); feedback->show();
    if (reportError) reportError(text);
}

void InspectionPanel::refresh() { request("inspect","state"); }

bool InspectionPanel::commitPendingCuts() {
    cutTimer->stop();
    return pendingCuts.isEmpty() || flushCuts();
}

bool InspectionPanel::setToolMode(const QString& mode) {
    const auto next=mode=="cuts"?QString("cuts"):QString("parts");
    if (next==toolMode) return true;
    if (!commitPendingCuts()) return false;
    toolMode=next;
    partsBox->setVisible(toolMode=="parts");
    if (auto* scroll=findChild<QScrollArea*>("inspectionScroll"))
        scroll->ensureWidgetVisible(cutsBox);
    return true;
}

void InspectionPanel::setState(const QString& nextId, const QJsonObject& next) {
    const auto previous=state;
    const bool firstUpdate=!stateKnown;
    const bool changedId=id!=nextId;
    const bool changedModel=changedId || previous["revision"]!=next["revision"];
    const bool changedBinding=changedModel || previous["dependency_revision"]!=next["dependency_revision"];
    const bool wasLoaded=loaded;
    id=nextId; state=next; loaded=state["loaded"].toBool();
    stateKnown=true;
    if (changedBinding || !loaded) { cutTimer->stop(); pendingCuts.clear(); }
    if (changedId) {
        ++partsGeneration; ++notesGeneration;
        partsRequested=false; notesRequested=false;
        partsLoading=false; notesLoading=false;
        parts.clear(); partItems.clear(); notes.clear(); noteItems.clear(); selectedNoteId.clear();
        const QSignalBlocker partBlocker(partTree), noteBlocker(noteTree);
        partTree->clear(); noteTree->clear();
        partsOffset=partsTotal=notesOffset=notesTotal=0;
    }
    if (changedBinding && !changedId && !previous["revision"].isUndefined()) {
        for (auto note=notes.begin(); note!=notes.end(); ++note) {
            const bool sameModel=note.value()["created_against"]==state["revision"];
            note.value()["binding"]=state["revision"].toString().isEmpty()?"unresolved":!sameModel?"stale":
                !note.value().contains("dependency_revision")?"unresolved":
                note.value()["dependency_revision"]==state["dependency_revision"]?"exact":"stale";
            if (auto* item=noteItems.value(note.key(),nullptr)) renderNote(item,note.value());
        }
    }
    const auto path=state["review_path"].toString();
    if (reviewPath->text()!=path) { reviewPath->setText(path); reviewPath->setCursorPosition(0); }
    reviewPath->setToolTip(path);
    copyReview->setEnabled(!path.isEmpty()); chooseReview->setEnabled(!id.isEmpty());
    returnButton->setEnabled(!id.isEmpty());
    availability->setText(loaded?"Click a visible surface or select a part below.":
        state["error"].toString(state["backend_error"].toString("The inspected model is not loaded. Your comment draft is kept.")));
    const auto reviewError=state["review_error"].toString();
    if (!reviewError.isEmpty()) { feedback->setText(reviewError); feedback->show(); }
    else if (!previous["review_error"].toString().isEmpty() && feedback->text()==previous["review_error"].toString()) {
        feedback->clear(); feedback->hide();
    }
    search->setEnabled(loaded); partTree->setEnabled(loaded);
    if (firstUpdate || changedModel || previous["hidden"]!=state["hidden"] || previous["can_undo"]!=state["can_undo"] || wasLoaded!=loaded) updateVisibility();
    if (firstUpdate || changedModel || previous["selected"]!=state["selected"] || previous["selection"]!=state["selection"] || previous["capabilities"]!=state["capabilities"] ||
        previous["hidden"]!=state["hidden"] || wasLoaded!=loaded) updateSelection();
    if (firstUpdate || changedBinding || previous["cuts"]!=state["cuts"] || previous["bounds"]!=state["bounds"] || wasLoaded!=loaded) updateCuts();
    updateCommentControls();
    updateNoteSelection();
    if (loaded && (!partsRequested || inventoryId!=id || inventoryRevision!=state["revision"])) beginParts();
    const bool documentChanged=!notesRequested || notesId!=id || notesPath!=path ||
        (state.contains("document_revision") && notesRevision!=state["document_revision"]) ||
        (state.contains("review_digest") && notesDigest!=state["review_digest"].toString());
    if (!id.isEmpty() && !path.isEmpty() && documentChanged) beginNotes();
    else if (path.isEmpty() && notesRequested) {
        ++notesGeneration; notesRequested=false; notesLoading=false; notesMore=false;
        notes.clear(); noteItems.clear(); notesOffset=notesTotal=0; selectedNoteId.clear();
        const QSignalBlocker blocker(noteTree); noteTree->clear(); updateNoteSelection();
    }
    updatePartsSummary(); updateNotesSummary();
}

QJsonObject InspectionPanel::selectedIdentity() const {
    const auto target=state["selected"].toString();
    auto identity=parts.value(target);
    const auto selection=state["selection"].toObject();
    for (auto item=selection.begin(); item!=selection.end(); ++item) identity[item.key()]=item.value();
    if (!target.isEmpty()) identity["id"]=target;
    return identity;
}

void InspectionPanel::updateSelection() {
    const auto selected=state["selected"].toString();
    const auto identity=selectedIdentity();
    const bool exact=!selected.isEmpty() && (parts.contains(selected) || !state["selection"].toObject().isEmpty());
    const auto selection=state["selection"].toObject();
    const bool unmapped=selection.contains("mapped") && !selection["mapped"].toBool();
    selectionName->setText(exact?partName(identity):selected.isEmpty() && !unmapped?"No part selected":"Source identity unavailable");
    selectionName->setToolTip(exact?partTooltip(identity,hidden.contains(selected)):QString());
    const auto exported=identity["exported_name"].toString();
    const auto anchor=identity["anchor"].toObject();
    QString hint;
    if (exact && !exported.isEmpty() && exported!=partName(identity)) hint="Exported: "+exported+"\n";
    if (exact) hint+=hidden.contains(selected)?"Hidden component. Use Show to restore it.":
        !anchor.isEmpty()?"Surface pin selected. A comment saves this point and view.":"Part selected. Click its surface to add a pin.";
    else hint="Select a part in the tree or click its surface. Whole-model comments remain available.";
    selectionHint->setText(hint);
    copyIdentity->setEnabled(loaded && exact);
    hidePart->setEnabled(loaded && exact && !hidden.contains(selected));
    showPart->setEnabled(loaded && exact && hidden.contains(selected));
    const bool complete=state["capabilities"].toObject()["identity"].toString()!="partial";
    isolatePart->setEnabled(loaded && exact && complete);
    isolatePart->setToolTip(complete?"Show only the selected component":"Isolate requires complete source identity mapping");
    const QSignalBlocker blocker(partTree);
    if (auto* item=partItems.value(selected,nullptr)) {
        if (partTree->currentItem()!=item) partTree->setCurrentItem(item);
    } else if (partTree->currentItem()) {
        partTree->setCurrentItem(nullptr); partTree->clearSelection();
    }
    updateCommentControls();
}

void InspectionPanel::updateVisibility() {
    hidden.clear();
    for (const auto& value:state["hidden"].toArray()) hidden.insert(value.toString());
    for (auto item=partItems.begin(); item!=partItems.end(); ++item) {
        const bool isHidden=hidden.contains(item.key());
        auto font=item.value()->font(0); font.setItalic(isHidden); item.value()->setFont(0,font);
        item.value()->setText(0,partName(parts.value(item.key()))+(isHidden?" (hidden)":""));
        item.value()->setToolTip(0,partTooltip(parts.value(item.key()),isHidden));
    }
    const bool canUndo=state.contains("can_undo")?state["can_undo"].toBool():true;
    undoVisibility->setEnabled(loaded && canUndo);
    restoreVisibility->setEnabled(loaded && !hidden.isEmpty());
}

void InspectionPanel::filterParts() {
    const auto query=search->text().trimmed();
    for (auto item=partItems.begin(); item!=partItems.end(); ++item) {
        const auto part=parts.value(item.key());
        bool match=query.isEmpty();
        for (const auto* field:{"name", "source_manufactured_label", "source_label", "exported_name", "path", "semantic_id", "id"})
            match=match || part[field].toString().contains(query,Qt::CaseInsensitive);
        item.value()->setHidden(!match);
    }
    updatePartsSummary();
}

void InspectionPanel::beginParts() {
    inventoryId=id; inventoryRevision=state["revision"]; partsRequested=true;
    partsLoading=true; partsMore=false; partsOffset=partsTotal=0;
    const auto generation=++partsGeneration;
    parts.clear(); partItems.clear();
    { const QSignalBlocker blocker(partTree); partTree->clear(); }
    QTimer::singleShot(0,this,[this,generation] { loadPartsPage(generation); });
}

void InspectionPanel::loadPartsPage(quint64 generation) {
    if (generation!=partsGeneration || inventoryId!=id) return;
    if (!loaded) { partsLoading=false; partsRequested=false; updatePartsSummary(); return; }
    const auto result=request("inspect","parts",{{"offset",partsOffset},{"limit",PageSize}},false);
    if (generation!=partsGeneration) return;
    partsLoading=false;
    if (!result["ok"].toBool()) { partsMore=true; updatePartsSummary(); return; }
    const auto items=result["items"].toArray();
    const int received=std::min(int(items.size()),PageSize);
    for (int index=0; index<received && parts.size()<RowLimit; ++index) {
        const auto part=items[index].toObject(); const auto target=part["id"].toString();
        if (target.isEmpty() || parts.contains(target)) continue;
        parts.insert(target,part);
        auto* item=new QTreeWidgetItem(partTree);
        item->setData(0,Qt::UserRole,target);
        partItems.insert(target,item);
    }
    partsOffset+=received;
    partsTotal=std::max(partsOffset,result["total"].toInt(partsOffset));
    partsMore=received>0 && result["has_more"].toBool(partsOffset<partsTotal);
    updateVisibility(); filterParts(); updateSelection();
    if (partsMore && parts.size()<RowLimit) {
        partsLoading=true;
        QTimer::singleShot(0,this,[this,generation] { loadPartsPage(generation); });
    } else partTree->sortItems(0,Qt::AscendingOrder);
    updatePartsSummary();
}

void InspectionPanel::updatePartsSummary() {
    int visible=0;
    for (auto* item:std::as_const(partItems)) if (!item->isHidden()) ++visible;
    QString summary=partsLoading?"Loading parts...":QString::number(parts.size())+" parts";
    if (partsTotal>parts.size()) summary=QString("%1 of %2 parts loaded").arg(parts.size()).arg(partsTotal);
    if (!search->text().trimmed().isEmpty()) summary+=QString(". %1 match").arg(visible);
    if (parts.size()>=RowLimit && partsMore) summary+=". Display limit reached; use the paginated CLI inventory for the rest.";
    if (!loaded && parts.isEmpty()) summary="Part inventory is available after the model loads.";
    partsSummary->setText(summary);
    moreParts->setVisible(partsMore && !partsLoading && parts.size()<RowLimit);
    moreParts->setEnabled(loaded && !partsLoading);
}

void InspectionPanel::updateCuts() {
    const auto bounds=state["bounds"].toArray();
    const auto values=state["cuts"].toArray();
    for (int index=0; index<3; ++index) {
        auto& cut=cuts[index];
        double low=bounds.size()==6?bounds[index*2].toDouble(-1):-1;
        double high=bounds.size()==6?bounds[index*2+1].toDouble(1):1;
        if (!std::isfinite(low) || !std::isfinite(high) || high<low) { low=-1; high=1; }
        if (high==low) { const auto padding=std::max(1.0,std::abs(low)*0.01); low-=padding; high+=padding; }
        cut.low=low; cut.high=high;
        QJsonObject value;
        for (const auto& candidate:values) if (candidate.toObject()["axis"].toString().toLower()==cut.axis) { value=candidate.toObject(); break; }
        const QSignalBlocker enableBlocker(cut.enabled), positionBlocker(cut.position), sliderBlocker(cut.slider), keepBlocker(cut.keep);
        cut.position->setRange(low,high);
        cut.position->setSingleStep(std::max(0.000001,(high-low)/CutSteps));
        if (!pendingCuts.contains(index) && !cut.slider->isSliderDown()) {
            cut.enabled->setChecked(value["enabled"].toBool());
            auto position=value["position"].toDouble((low+high)/2);
            if (!std::isfinite(position)) position=(low+high)/2;
            cut.position->setValue(position);
            cut.slider->setValue(qRound(std::clamp((position-low)/(high-low),0.0,1.0)*CutSteps));
            cut.keep->setCurrentIndex(value["keep"].toString()=="negative"?1:0);
        }
        const auto tooltip=QString("Model %1 bounds: %2 to %3. Position uses model units.")
            .arg(cut.axis.toUpper()).arg(low,0,'g',8).arg(high,0,'g',8);
        cut.position->setToolTip(tooltip); cut.slider->setToolTip(tooltip);
        cut.enabled->setEnabled(loaded); cut.position->setEnabled(loaded);
        cut.keep->setEnabled(loaded); cut.slider->setEnabled(loaded);
    }
    resetCutButton->setEnabled(loaded);
}

void InspectionPanel::queueCut(int index) {
    if (!loaded) return;
    pendingCuts.insert(index);
    if (!cutTimer->isActive()) cutTimer->start();
}

bool InspectionPanel::flushCuts() {
    if (!loaded || id.isEmpty()) { pendingCuts.clear(); return false; }
    const auto currentId=id; const auto revision=state["revision"], dependencies=state["dependency_revision"];
    std::array<QJsonObject,3> requests;
    const auto pending=std::exchange(pendingCuts,{});
    for (const auto index:pending) {
        const auto& cut=cuts[index];
        requests[index]={{"axis",cut.axis},{"enabled",cut.enabled->isChecked()},
            {"position",cut.position->value()},{"keep",cut.keep->currentData().toString()}};
    }
    for (int index=0; index<3; ++index) if (pending.contains(index)) {
        if (!loaded || id!=currentId || state["revision"]!=revision || state["dependency_revision"]!=dependencies) return false;
        if (!request("inspect","cut",requests[index])["ok"].toBool()) { updateCuts(); return false; }
    }
    return loaded && id==currentId && state["revision"]==revision && state["dependency_revision"]==dependencies;
}

void InspectionPanel::resetCuts() {
    if (!loaded) return;
    for (int index=0; index<3; ++index) {
        auto& cut=cuts[index];
        const QSignalBlocker enableBlocker(cut.enabled), positionBlocker(cut.position), sliderBlocker(cut.slider), keepBlocker(cut.keep);
        cut.enabled->setChecked(false); cut.position->setValue((cut.low+cut.high)/2);
        cut.slider->setValue(CutSteps/2); cut.keep->setCurrentIndex(0);
        pendingCuts.insert(index);
    }
    if (!cutTimer->isActive()) cutTimer->start(0);
}

void InspectionPanel::beginNotes() {
    const bool sameDocument=notesId==id && notesPath==state["review_path"].toString();
    notesId=id; notesPath=state["review_path"].toString(); notesRevision=state["document_revision"];
    notesDigest=state["review_digest"].toString(); notesRequested=true;
    notesLoading=true; notesMore=false; notesOffset=notesTotal=0;
    const auto generation=++notesGeneration;
    if (!sameDocument) {
        notes.clear(); noteItems.clear();
        const QSignalBlocker blocker(noteTree); noteTree->clear();
    }
    updateNoteSelection();
    QTimer::singleShot(0,this,[this,generation] { loadNotesPage(generation); });
}

void InspectionPanel::loadNotesPage(quint64 generation) {
    if (generation!=notesGeneration || notesId!=id) return;
    const auto result=request("annotations","list",{{"offset",notesOffset},{"limit",PageSize}},false);
    if (generation!=notesGeneration) return;
    notesLoading=false;
    if (!result["ok"].toBool()) { notesMore=true; updateNotesSummary(); return; }
    if (result.contains("document_revision")) notesRevision=result["document_revision"];
    if (result.contains("review_digest")) notesDigest=result["review_digest"].toString();
    const auto items=result["items"].toArray();
    const int received=std::min(int(items.size()),PageSize);
    const QSignalBlocker blocker(noteTree);
    // Publish the replacement only after a valid first page. A malformed or
    // temporarily unreadable review keeps the last valid list and the draft.
    if (notesOffset==0) { notes.clear(); noteItems.clear(); noteTree->clear(); }
    for (int index=0; index<received && notes.size()<RowLimit; ++index) {
        auto note=items[index].toObject();
        if (note["note"].isObject()) {
            const auto outer=note; note=outer["note"].toObject();
            if (outer.contains("binding")) note["binding"]=outer["binding"];
        }
        const auto noteId=note["id"].toString();
        if (noteId.isEmpty() || notes.contains(noteId)) continue;
        notes.insert(noteId,note);
        auto* item=new QTreeWidgetItem(noteTree);
        item->setData(0,Qt::UserRole,noteId);
        renderNote(item,note);
        noteItems.insert(noteId,item);
        if (selectedNoteId==noteId) noteTree->setCurrentItem(item);
    }
    notesOffset+=received;
    notesTotal=std::max(notesOffset,result["total"].toInt(notesOffset));
    notesMore=received>0 && result["has_more"].toBool(notesOffset<notesTotal);
    updateNoteSelection(); updateNotesSummary(); updateCommentControls();
}

QJsonObject InspectionPanel::selectedNote() const { return notes.value(selectedNoteId); }

void InspectionPanel::updateNoteSelection() {
    const auto note=selectedNote(); const bool available=!note.isEmpty();
    const bool exact=binding(note)=="exact";
    const bool writable=state["review_error"].toString().isEmpty();
    restoreNote->setEnabled(loaded && available && exact && writable);
    restoreNote->setToolTip(available && !exact?"This comment is stale for the displayed model. Its saved view cannot be restored.":
        "Restore the selected comment's saved view and selection");
    resolveNote->setEnabled(available && writable); deleteNote->setEnabled(available && writable);
    const auto status=note["status"].toString();
    const bool reopen=status=="resolved" || status=="obsolete";
    resolveNote->setText(reopen?"Reopen":"Resolve");
    resolveNote->setAccessibleName(reopen?"Reopen the selected comment":"Resolve the selected comment");
    resolveNote->setToolTip(resolveNote->accessibleName());
    noteText->setVisible(available);
    if (noteText->toPlainText()!=note["text"].toString()) noteText->setPlainText(note["text"].toString());
}

void InspectionPanel::updateNotesSummary() {
    QString summary=notesLoading?"Loading comments...":QString::number(notes.size())+" comments";
    if (notesTotal>notes.size()) summary=QString("%1 of %2 comments loaded").arg(notes.size()).arg(notesTotal);
    if (notes.size()>=RowLimit && notesMore) summary+=". Display limit reached; use the paginated CLI list for the rest.";
    if (state["review_path"].toString().isEmpty()) summary="Choose a review document to save comments.";
    notesSummary->setText(summary);
    moreNotes->setVisible(notesMore && notes.size()<RowLimit);
    moreNotes->setEnabled(!notesLoading && !id.isEmpty());
}

QJsonObject InspectionPanel::expectedDocument() const {
    return {{"expected_revision",notesRevision.isUndefined()?state["document_revision"]:notesRevision},
        {"expected_digest",notesDigest}};
}

void InspectionPanel::updateCommentControls() {
    const auto bytes=comment->toPlainText().toUtf8().size();
    const bool model=commentScope->currentData().toString()=="model";
    const bool selected=!state["selected"].toString().isEmpty();
    const bool text=!comment->toPlainText().trimmed().isEmpty();
    commentSize->setText(QString("%1 / %2 bytes").arg(bytes).arg(NoteBytes));
    if (model) commentTarget->setText("Whole-model comment. Saving also records the current view.");
    else if (selected) commentTarget->setText("Target: "+partName(selectedIdentity())+". A model surface click can place a pin.");
    else commentTarget->setText("Select a part or choose Whole model. A model surface click can place a pin.");
    addButton->setEnabled(loaded && state["review_error"].toString().isEmpty() && !state["review_path"].toString().isEmpty() && text && bytes<=NoteBytes && (model || selected));
    addButton->setToolTip(bytes>NoteBytes?"The comment exceeds the 8,192-byte limit. Your draft is kept.":
        !loaded?"Load the model before saving a comment. Your draft is kept.":"Save a comment with the current view");
    commentScope->setEnabled(loaded); commentStatus->setEnabled(loaded);
}

void InspectionPanel::addComment() {
    if (!addButton->isEnabled()) return;
    // A numeric edit may commit when Add takes focus, before the queued cut is
    // sent. Publish it first so the saved view matches the controls the user saw.
    if (!pendingCuts.isEmpty()) {
        cutTimer->stop();
        if (!flushCuts() || !addButton->isEnabled()) return;
    }
    const auto draft=comment->toPlainText();
    QJsonObject note{{"text",draft},{"status",commentStatus->currentData().toString()}};
    if (commentScope->currentData().toString()=="model") note["targets"]=QJsonArray();
    auto fields=expectedDocument(); fields["note"]=note;
    const auto result=request("annotations","add",fields);
    if (result["ok"].toBool() && comment->toPlainText()==draft) comment->clear();
}
