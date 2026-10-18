#pragma once

#include <QString>

// Where agentin keeps its files (organization "siplasplas", set in main()): configuration, what the
// user chooses (settings.json, approvals.json), in ~/.config/siplasplas/agentin; data, what agentin
// produces (conversation indexes, open tabs, recent directories, turn locks, the crash log, the managed
// Claude environment), in ~/.local/share/siplasplas/agentin. Without an organization (older versions'
// places) both are ~/.local/share/agentin.
QString appConfigDirectory();
QString appDataDirectory();

// Another agentin process runs now (found through /proc); its files must not be moved under it.
bool otherAgentinRunning();
