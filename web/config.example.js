// Template for web/config.js, which is GITIGNORED and holds the real values.
//
// Generate it from include/secret.h, so the firmware and the dashboard cannot
// drift apart on which project they point at:
//
//     python tools/make_web_config.py
//
// NEVER put the service_role key here. It carries BYPASSRLS, which makes every
// row-level policy in supabase/schema.sql decorative.

window.BOWLSTACK_CONFIG = {
  // Project URL, no trailing slash. Supabase -> Project Settings -> API.
  url: '',

  // The anon (public) key — the same key every flashed board holds.
  anonKey: '',

  // How the dashboard authenticates.
  //
  // Reads require the `authenticated` role. `anon` is deliberately write-only
  // (supabase/schema.sql §8: a device must never be able to read another
  // installation's telemetry), so WITHOUT one of these every query returns an
  // empty array rather than an error — which looks exactly like a dead fleet.
  //
  //   'anonymous'          silent anonymous sign-in, no prompt, no account.
  //                        Needs Authentication -> Sign In / Providers ->
  //                        "Allow anonymous sign-ins" turned ON.
  //   { email, password }  silent sign-in as a real account you created.
  //   null                 show the sign-in form.
  autoLogin: 'anonymous',
};
