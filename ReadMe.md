# Source Demo Render
The program can be downloaded [here](https://github.com/crashfort/SourceDemoRender/releases). Visit [here](https://twitch.streamlabs.com/crashfort/) if you wish to support the development.

SDR is a multithreaded, hardware accelerated solution to create high quality movies for the Source engine.

## Prerequisites
Any DirectX 11 (Direct3D 11.0) compatible adapter with minimum of Windows 7 is required. If you wish to not use **sdr_d3d11_staging**, Windows 8.1 or later is required.

## Installing
SDR comes in separate singleplayer and multiplayer versions which will only work with whatever SDK the game was built with. `SourceDemoRender.Multiplayer.dll` is for games such as Counter-Strike: Source and `SourceDemoRender.Singleplayer.dll` targets for example Half-Life 2.

The content of the archive should go in the root game directory. Examples:

* steamapps\common\Counter-Strike Source\cstrike\
* steamapps\common\Half-Life 2\hl2\

You can use the ``.cfg`` files that comes with SDR as a base where to add your own preferred settings. The loader files are the preferred way to load SDR. It is executed as follows:

* `exec sdr_load_mp` - For multiplayer games
* `exec sdr_load_sp` - For singleplayer games

The plugin can be loaded at the main menu or in demo playback, but must be before any call to `startmovie`. It's required to unload SDR after you are done recording a particular map as the state does not carry over between demos.

**You need to launch with -insecure for Source to be able to load plugins.**

## Instructions
When you are ready to create your movie just type `startmovie <name>` and then `endmovie` as usual. **Do not exit the game until you see a green message that says the movie is completed.**

Example of supported video containers:

* `startmovie test.avi`
* `startmovie test.mp4`
* `startmovie test.mov`
* `startmovie test.mkv`

The default video encoder is ``libx264``. Other available is ``libx264rgb`` which will produce an RGB video with no color loss. Note however that ``libx264rgb`` encodes slower than ``libx264`` and will greatly increase file size. Some video editors are not capable of opening libx264rgb encoded videos or MKV containers.

## Guide

SDR can output in YUV420, YUV444 and BGR0 formats with x264. Color space can be `601` or `709`, for YUV video the color range is `full`.

### Vegas Pro

This video editor cannot open:
* YUV444 or RGB video
* MKV containers - *Use MP4*
* Vegas Pro 14: `CRF 0` with `ultrafast` - *Use higher CRF or slower preset*
* Vegas Pro 14: AVI containers with YUV video - *Use MP4*

If you wish to open an RGB video you have to do a workaround:

* Get [ffmpeg](https://ffmpeg.org/)
* Run `.\ffmpeg.exe -i .\input.mp4 -vcodec huffyuv -pix_fmt rgb24 output.avi`
* Above will create a VFW compatible file that [VirtualDub](http://virtualdub.org/) can open
* Save with optional lossless encoder (not Lagarith) or uncompressed and open in Vegas Pro

If you just use YUV420 you have to use the `709` color space. If you also want to render using `x264vfw`, you have to set these advanced parameters to not lose any color:

`--colormatrix=bt709 --transfer=bt709 --colorprim=bt709 --range=pc`

### Adobe Premiere

This video editor can open `libx264rgb` videos natively which is the recommended way as there are no possibilities of color loss. If you want to use YUV video you should use the `601` color space.

## General commands

<table>
	<thead>
		<th>Name</th>
		<th>Description</th>
	</thead>
	<tbody>
	<tr>
		<td>sdr_update</td>
		<td>
			Check to see if there are any updates available. Library updates need to be manual but game configurations are updated automatically with this command.
		</td>
	</tr>
	<tr>
		<td>sdr_version</td>
		<td>
			Displays the current library and game config versions.
		</td>
	</tr>
	</tbody>
</table>

## General variables

<table>
	<thead>
		<th>Name</th>
		<th>Description</th>
	</thead>
	<tbody>
	<tr>
		<td>sdr_outputdir</td>
		<td>
			Path where to save the video. The directory structure must exist. This cannot be the root of a drive, it must be a in at least one directory. If this is empty, the output will be in the game root.
		</td>
	</tr>
	<tr>
		<td>sdr_render_framerate</td>
		<td>
			Movie output framerate.
            <br/><br/>
            <b>Values:</b> 30 to 1000 <br/>
            <b>Default:</b> 60 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_endmovieflash</td>
		<td>
			Flash the window when endmovie gets called. This can be used with the demo director to do "endmovie" on a certain tick so you don't have to keep looking at the window.
			<br/><br/>
			<b>Values:</b> 0 or 1 <br/>
            <b>Default:</b> 0 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_endmoviequit</td>
		<td>
			Quits the game after all processing is done.
			<br/><br/>
			<b>Values:</b> 0 or 1 <br/>
            <b>Default:</b> 0 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_game_suppressdebug</td>
		<td>
			Prevents engine output debug messages to reach the operating system.
			<br/><br/>
			<b>Values:</b> 0 or 1 <br/>
            <b>Default:</b> 1 <br/>
		</td>
	</tr>
	</tbody>
</table>

## Video variables

<table>
	<thead>
		<th>Name</th>
		<th>Description</th>
	</thead>
	<tbody>
	<tr>
		<td>sdr_movie_suppresslog</td>
		<td>
			Enable or disable log output from LAV.
            <br/><br/>
            <b>Values:</b> 0 or 1 <br/>
            <b>Default:</b> 1 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_movie_encoder</td>
		<td>
			Desired video encoder.
			<br/><br/>
			<b>Values:</b> libx264, libx264rgb <br/>
            <b>Default:</b> libx264 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_movie_encoder_pxformat</td>
		<td>
			Encoded pixel format to use.
			<br/><br/>
			<b>Values:</b><br/>
			<table>
				<thead>
				<tr>
						<th>Encoder</th>
						<th>Values</th>
					</tr>
				</thead>
				<tbody>
					<tr>
						<td>libx264</td>
						<td>i420, i444</td>
					</tr>
					<tr>
						<td>libx264rgb</td>
						<td>bgr0</td>
					</tr>
				</tbody>
			</table>
			<br/>
            <b>Default:</b> First listed above per encoder <br/>
			<a href="https://wiki.videolan.org/YUV/">Read more about YUV</a>
		</td>
	</tr>
	<tr>
		<td>sdr_d3d11_staging</td>
		<td>
			Use extra intermediate buffer when retreiving data from the GPU.
			<br/><br/>
            <b>Values:</b> 0 or 1 <br/>
            <b>Default:</b> 1 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_x264_crf</td>
		<td>
			Constant rate factor quality value. Note that using 0 (lossless) can produce a video with a 4:4:4 profile which your media player might not support.
			<br/><br/>
            <b>Values:</b> 0 to 51 <br/>
            <b>Default:</b> 0 <br/>
			<a href="https://trac.ffmpeg.org/wiki/Encode/H.264">Read more</a>
		</td>
	</tr>
	<tr>
		<td>sdr_x264_preset</td>
		<td>
			Encoding preset. If you can, prefer not to use a slow encoding preset as the encoding may fall behind and the game will have to wait for it to catch up.
			<br/><br/>
            <b>Default:</b> ultrafast <br/>
			<a href="https://trac.ffmpeg.org/wiki/Encode/H.264">Read more</a>
		</td>
	</tr>
	<tr>
		<td>sdr_x264_intra</td>
		<td>
			Whether to produce a video of only keyframes.
			<br/><br/>
            <b>Values:</b> 0 or 1 <br/>
            <b>Default:</b> 1 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_movie_encoder_colorspace</td>
		<td>
			YUV color space. This value is handled differently in media, try experimenting. Not available in RGB video.
			<br/><br/>
            <b>Values:</b> 601 or 709 <br/>
            <b>Default:</b> 709 <br/>
		</td>
	</tr>
	</tbody>
</table>

## Sampling variables

<table>
	<thead>
		<th>Name</th>
		<th>Description</th>
	</thead>
	<tbody>
	<tr>
		<td>sdr_sample_mult</td>
		<td>
			Value to multiply with <b>sdr_render_framerate</b>. This is how many frames will be put together to form a final frame multiplied by exposure. Less than 2 will disable sampling.
            <br/><br/>
            <b>Values:</b> Over 0 <br/>
            <b>Default:</b> 32 <br/>
		</td>
	</tr>
	<tr>
		<td>sdr_sample_exposure</td>
		<td>
			Fraction of time per frame that is exposed for sampling
            <br/><br/>
            <b>Values:</b> 0 to 1 <br/>
            <b>Default:</b> 0.5 <br/>
		</td>
	</tr>
	</tbody>
</table>

