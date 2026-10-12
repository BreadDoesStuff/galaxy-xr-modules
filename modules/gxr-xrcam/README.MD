# Cam Debug (xrcam)
Streams all **13 physical headset cameras** as MJPEG over HTTP:
`http://<headset-ip>:8080/`.  

All 13 streams run at the same time at the sensors' native rates:
RGB ~65-68 fps, tracking 60 fps, eye 30 fps, ToF 10 fps. It starts at boot, and no system files are modified.