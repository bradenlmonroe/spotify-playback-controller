The overall goal of the project was to create a better way to control my Spotify listening experience.

Music plays a large part in my life, and I rely on having music in the background to get work done.  However, many of the applications I use on the desktop have the same issue - if i want to interact with the app in any way,  to skip the current song, to look at the artist, to even pause it, requires tabbing through all of my screens and getting lost in pages.  If I leave the window open, I lose valuable monitor real estate.

I then set out to find a solution that would allow me to control my music without dedicating a whole monitor to the application.  Although other solutions exist, including modding a depreciated Spotify Car Thing, I concluded that these solutions were outside the scope of what I wanted the project to accomplish.

After going back and forth between a few options, I settled on using an ESP32 as the microcontroller.  I didn't need anything as powerful as a Raspberry Pi 4 or the STM32, and the ESP offered easily plug-and-play operation with the already-required type-C cable connection.

The rotary encoder was a leftover piece from another project, and has added integrations including the push-button capability and the LED embedded in the handle.

The monitor was selected for its size and cost. Additionally, it boasted not supporting touchscreen, which is a benefit for this project due to the rotary encoder being the main source of control.