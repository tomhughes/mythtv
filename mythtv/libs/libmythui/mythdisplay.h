#ifndef MYTHDISPLAY_H
#define MYTHDISPLAY_H

// Qt
#include <QSize>
#include <QScreen>
#include <QMutex>

// MythTV
#include "mythuiexp.h"
#include "referencecounter.h"

// Std
#include <cmath>

#define VALID_RATE(rate) ((rate) > 20.0F && (rate) < 200.0F)

class DisplayInfo
{
  public:
    DisplayInfo(void) = default;
    explicit DisplayInfo(int Rate)
      : m_rate(Rate)
    {
    }

    int Rate(void) const
    {
        return static_cast<int>(lroundf(m_rate));
    }

    QSize m_size { 0, 0};
    QSize m_res  { 0, 0};
    float m_rate { -1 };
};

class MUI_PUBLIC MythDisplay : public QObject, public ReferenceCounter
{
    Q_OBJECT

  public:
    static MythDisplay* AcquireRelease(bool Acquire = true);

    void     SetWidget        (QWidget *MainWindow);
    QScreen* GetCurrentScreen (void);
    int      GetScreenCount   (void);
    double   GetPixelAspectRatio(void);

    virtual DisplayInfo GetDisplayInfo(int VideoRate = 0);
    static bool         SpanAllScreens(void);
    static QString      GetExtraScreenInfo(QScreen *qScreen);

  public slots:
    void ScreenChanged        (QScreen *qScreen);
    void PrimaryScreenChanged (QScreen *qScreen);
    void ScreenAdded          (QScreen *qScreen);
    void ScreenRemoved        (QScreen *qScreen);

  signals:
    void CurrentScreenChanged (QScreen *Screen);
    void ScreenCountChanged   (int Screens);

  protected:
    MythDisplay();
    virtual ~MythDisplay();
    QScreen*     GetDesiredScreen   (void);
    static void  DebugScreen        (QScreen *qScreen, const QString &Message);
    static float SanitiseRefreshRate(int Rate);

    QWidget* m_widget      { nullptr };
    QScreen* m_screen      { nullptr };
    QMutex   m_screenLock  { QMutex::Recursive };

  private:
    Q_DISABLE_COPY(MythDisplay)
};

#endif // MYTHDISPLAY_H
