
## M4-NOTES update (2026-10-23 01:xx-02:3x)

### v0.0.20-0.0.27 tiến trình
- EDID pipeline HOÀN TOÀN: hasDDCConnect=true + getDDCBlock trả gHmlEDID
  (kIODDCBlockTypeEDID block 1) → IODisplay tự đọc + parse → 
  IODisplayPrefsKey có EDID hash + IODisplayEDID trên AppleDisplay ✓
  (cách CHUẨN Apple, props không phải set tay vào registry)
- getTimingInfoForDisplayMode → appleTimingID=42 (FixedRateLCD)
- getVRAMRange override
- v0.0.27 timer repeat OK: setTimeoutMS+enable trong callback → 
  1031 ticks/6m (66ms)
- Quan trọng: CGXConstructVirtualFramebuffer offline + IOPresentment
  'Unable to acquire map' là NORMAL (cả GUI vmware boot cũng có) —
  KHÔNG phải blocker
- ROOT INSIGHT: WindowServer ĐANG VẼ vào fb kext (fb white/gradient,
  không còn pattern bytes 0xce3100ff) — scanout vẫn stale pattern
- Còn lại: flushWholeFb không emit TRANSFER_TO_HOST_2D (56B)/RESOURCE
  _FLUSH trong ticks — log chỉ thấy GET_DISPLAY_INFO 24B ×2937 —
  debug submitCtrl path/scanoutAttached guard trong tick context
- Approve recipe chuẩn: quit Settings → deep-link ?General → 
  kextload (trigger banner) → zoom button 2 (AppleScript) → 
  Allow @(941,586 hoặc 942,693 tùy window size) → password @(640,455)
  → Modify @(640,503)
- OC picker: click vào vùng trống để lấy keyboard focus rồi 
  sendkey right ×2 (mỗi key riêng) + kp_enter
