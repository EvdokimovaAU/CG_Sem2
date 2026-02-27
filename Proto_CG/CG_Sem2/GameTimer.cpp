#include "GameTimer.h"
#include <windows.h>

GameTimer::GameTimer()
{
    int64_t countsPerSec = 0;
    QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
    m_secondsPerCount = 1.0 / (double)countsPerSec;
    Reset();
}

void GameTimer::Reset()
{
    int64_t t = 0;
    QueryPerformanceCounter((LARGE_INTEGER*)&t);
    m_prevTime = t;
    m_totalTime = 0.0;
    m_deltaTime = 0.0f;
}

void GameTimer::Tick()
{
    int64_t t = 0;
    QueryPerformanceCounter((LARGE_INTEGER*)&t);

    double dt = (t - m_prevTime) * m_secondsPerCount;
    m_prevTime = t;

    if (dt < 0.0) dt = 0.0;

    m_deltaTime = (float)dt;
    m_totalTime += dt;
}
