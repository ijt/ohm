#include <QTest>

#include "UrlMatch.h"

using shinto::matchUrl;
using shinto::UrlMatchTier;

Q_DECLARE_METATYPE(UrlMatchTier)

class UrlMatchTest : public QObject {
  Q_OBJECT

 private slots:
  void tiers_data() {
    QTest::addColumn<QString>("target");
    QTest::addColumn<QString>("needle");
    QTest::addColumn<UrlMatchTier>("want");

    const QString repo = QStringLiteral("github.com/ijt/shinto");
    QTest::newRow("host prefix") << repo << "git" << UrlMatchTier::Prefix;
    QTest::newRow("case-insensitive") << repo << "GIT" << UrlMatchTier::Prefix;
    QTest::newRow("path segment") << repo << "shinto" << UrlMatchTier::SegmentStart;
    QTest::newRow("across segments") << repo << "ijt/sh" << UrlMatchTier::SegmentStart;
    QTest::newRow("mid-word") << repo << "hint" << UrlMatchTier::Substring;
    QTest::newRow("dropped letter") << repo << "shnto" << UrlMatchTier::Scattered;
    QTest::newRow("too spread out") << repo << "gso" << UrlMatchTier::None;
    QTest::newRow("too short to scatter") << repo << "gs" << UrlMatchTier::None;
    QTest::newRow("absent") << repo << "zzz" << UrlMatchTier::None;
    QTest::newRow("empty") << repo << "" << UrlMatchTier::None;
    // A later occurrence at a segment start beats an earlier mid-word one.
    QTest::newRow("best occurrence") << "xhub.com/hub" << "hub" << UrlMatchTier::SegmentStart;
    // Query strings only count for a plain prefix match.
    QTest::newRow("query ignored") << "example.com/a?q=shinto" << "shinto" << UrlMatchTier::None;
    QTest::newRow("fragment ignored") << "example.com/a#shinto" << "shinto" << UrlMatchTier::None;
    QTest::newRow("query in prefix")
        << "example.com/a?q=shinto" << "example.com/a?q=sh" << UrlMatchTier::Prefix;
  }

  void tiers() {
    QFETCH(QString, target);
    QFETCH(QString, needle);
    QFETCH(UrlMatchTier, want);
    QCOMPARE(matchUrl(target, needle), want);
  }
};

QTEST_APPLESS_MAIN(UrlMatchTest)
#include "UrlMatchTest.moc"
