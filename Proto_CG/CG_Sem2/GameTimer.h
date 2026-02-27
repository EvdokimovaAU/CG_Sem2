#pragma once
#include <cstdint>

class GameTimer
{
public:
    GameTimer();

    void Reset();
    void Tick();

    float DeltaTime() const { return m_deltaTime; }
    float TotalTime() const { return (float)m_totalTime; }

private:
    double  m_secondsPerCount = 0.0;
    int64_t m_prevTime = 0;

    double  m_totalTime = 0.0;
    float   m_deltaTime = 0.0f;
};
