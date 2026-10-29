// FRAME RHYTHM - the clock of the animator, tied to the sweep of panel.
//
// THE PROBLEM. The periodic app was redesigned every periodo_ms "of clock",
// and the panel changes pages at HIS pace (15.7 to 20.6 ms, depending on the clock
// of panel - Settings > Display). The two don't match: one app of 40 ms was
// almost always 2 on-screen scans and, from time to time, 3 - one in stride
// periodic in an animation that the rest of the time was smooth. It was seen on screen
// of blocking, but it was valid for all animated apps (Radar, Cobra, IMU...).
//
// THE RULE. The period of app becomes a whole number of scans (the most
// close to what he asked for, never less than one), and the frame starts right after
// of a scan: it takes almost a full period to get ready and goes on
// always air in the same phase. No app changes a line to win this.
//
// AND THE MOVEMENT. Those who animate per team can requestsistema_quadro_dt() (app.h):
// the distance between the scans in which this frame and the previous one GO AIR -
// not between the moments in which they were drawn. So what appears walked
// exactly the time that became on screen, even if the drawing was delayed a little.
//
// Only the of interface task calls this.

#pragma once

#include <stdint.h>

// Called at the BEGINNING of every pass of drawing (desenhar_conteudo), whatever
// for the reason. 'owner' identifies who draws (the app, the home, the lock):
// changed owner, the clock restarts. 'periodo_ms' = the period of now (0 =
// is not periodic: nothing is scheduled).
void pacing_frame(const void* dono, uint32_t periodo_ms);

// How much is left, in months, until the next periodic frame (0 = already past the hour).
uint32_t pacing_falta_ms(void);

// dt(s) of frame in progress - see top. Always > 0; whoever encourages limits
// in a way that suits you (one app that became stopped 5 s gets 5 s).
float pacing_dt(void);
