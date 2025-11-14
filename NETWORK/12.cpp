//#include <QApplication>
//#include <QLabel>
//#include <QLineEdit>
//#include <QPushButton>
//#include <QVBoxLayout>
//#include <QWidget>
//
//int main(int argc, char* argv[]) {
//    QApplication app(argc, argv);
//
//    QLineEdit* nameField = new QLineEdit;
//    QLabel* greeting = new QLabel;
//    QPushButton* button = new QPushButton("È®ÀÎ");
//
//    QObject::connect(button, &QPushButton::clicked, [=]() {
//        QString name = nameField->text();
//        greeting->setText("¾È³çÇÏ¼¼¿ä, " + name + "´Ô!");
//        });
//
//    QVBoxLayout* layout = new QVBoxLayout;
//    layout->addWidget(nameField);
//    layout->addWidget(button);
//    layout->addWidget(greeting);
//
//    QWidget window;
//    window.setLayout(layout);
//    window.show();
//
//    return app.exec();
//}