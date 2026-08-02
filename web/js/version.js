// Dashboard version, shown in the header and stamped into every diagnostics
// copy. Bump it on any deploy the field trial should be able to tell apart --
// during a trial the code changes daily, and "which build is this tablet on"
// is otherwise unanswerable from a photo of the screen.
//
// Separate from the firmware's include/version.h: they ship independently.
export const APP_VERSION = '1.17';
