import subprocess, sys, json, statistics as st
f = sys.argv[1]
j = json.loads(subprocess.check_output(['ffprobe','-v','error','-show_entries','stream=nb_frames,duration,avg_frame_rate,r_frame_rate:packet=pts_time','-of','json',f]))
pk = [float(p['pts_time']) for p in j['packets']]
d = [b-a for a,b in zip(pk,pk[1:])]
s = j['streams'][0]
print(f"{f}: frames={len(pk)} stream_dur={s.get('duration')} avg_rate={s['avg_frame_rate']} span={pk[-1]-pk[0]:.2f}s")
print(f"  pts delta ms: median={st.median(d)*1000:.1f} p5={sorted(d)[int(len(d)*.05)]*1000:.1f} p95={sorted(d)[int(len(d)*.95)]*1000:.1f} max={max(d)*1000:.0f} gaps>100ms={sum(1 for x in d if x>0.1)} monotonic={all(x>0 for x in d)}")
r = subprocess.run(['ffmpeg','-v','error','-i',f,'-f','null','-'],capture_output=True,text=True)
print("  full decode errors:", r.stderr.strip() or "none")
