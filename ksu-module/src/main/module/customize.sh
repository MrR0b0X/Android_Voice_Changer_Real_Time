#!/system/bin/sh
# AVCRT module installer — system-wide voice changer.
#   • App voice change (Zygisk hook): works on ANY device with Magisk+Zygisk, no
#     kernel patch needed — covers Telegram / VoIP / recorders / voice messages.
#   • Cellular GSM clean-replace (avcrtd daemon): ONLY on a device flashed with the
#     AVCRT-patched kernel (adds the "AVCRT Dec Mute" mixer control). Without it the
#     daemon detects the missing control and leaves calls untouched — app voice
#     change still works.

ui_print "  AVCRT — system-wide voice changer"
ui_print "  • All apps (Zygisk): works everywhere"
ui_print "  • Cellular calls: requires a matching AVCRT-patched audio kernel"
ui_print "  • Supported cards: litomtpsndcard (lito-mtp-snd-card), lahainayupikidp (lahaina-yupikidp-snd-card)"
ui_print "  • Cellular routing remains experimental"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/avcrtd"        0 0 0755
set_perm "$MODPATH/bin/avcrt_inject"  0 0 0755
set_perm "$MODPATH/service.sh"        0 0 0755
set_perm "$MODPATH/post-fs-data.sh"   0 0 0755

# Fresh install = engine OFF until the app explicitly enables it. Reset the
# active-preset config to disabled (a leftover enabled=true would make the daemon
# engage on the first boot before the app has synced its state → voice changed
# even though the app shows it off). The app rewrites this when the user picks a
# preset / toggles the engine.
mkdir -p /data/local/tmp/avcrt
echo '{"schema":1,"enabled":false,"presetId":null,"dsp":null,"revision":0}' \
    > /data/local/tmp/avcrt/active_preset.json
echo "off" > /data/local/tmp/avcrt/engine_state
chmod 0755 /data/local/tmp/avcrt
chmod 0644 /data/local/tmp/avcrt/active_preset.json /data/local/tmp/avcrt/engine_state

touch /data/adb/avcrt_install_pending
ui_print "  Reboot to activate (engine starts OFF — enable it in the app)."
