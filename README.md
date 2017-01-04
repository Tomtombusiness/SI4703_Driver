rpi-si4703
==========

Control FM Breackoutboard Si4703 with Raspberry Pi

Usage
=====

You have to install <a href="http://wiringpi.com/download-and-install/">wiringPi</a> first. SDA/SDIO and Reset from the breakout board should be wired to GPIO0 and GPIO23 respectively.
<pre>
	<code>
git clone https://github.com/Tomtombusiness/SI4703_Driver.git
cd rpi-si4703/
gcc -o Radio example/Radio.cpp Si4703_Breakout.cpp -lwiringPi
sudo ./Radio
	</code>
</pre>


