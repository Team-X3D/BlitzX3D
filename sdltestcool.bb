Graphics3D 800,600,0,2
SetBuffer BackBuffer()

camera = CreateCamera()
PositionEntity camera, 0, 0, -5

light = CreateLight()
RotateEntity light, 45, 45, 0

t0 = CreateTexture(64, 64)
SetBuffer TextureBuffer(t0)
For y = 0 To 63
	For x = 0 To 63
		If ((x / 8 + y / 8) Mod 2) = 0 Then WritePixel x, y, $FFFFFFFF Else WritePixel x, y, $FFD0D0D0
	Next
Next

t1 = CreateTexture(64, 64)
SetBuffer TextureBuffer(t1)
For y = 0 To 63
	For x = 0 To 63
		WritePixel x, y, (255 Shl 24) Or ((160 + x) Shl 16) Or ((160 + y) Shl 8) Or 200
	Next
Next

t2 = CreateTexture(64, 64)
SetBuffer TextureBuffer(t2)
For y = 0 To 63
	For x = 0 To 63
		If (x Mod 16) < 8 Then WritePixel x, y, $FF080808 Else WritePixel x, y, $FF282828
	Next
Next
SetBuffer BackBuffer()

dt = CreateTexture(800, 600)
SetBufferDepth BackBuffer(), TextureBuffer(dt)

b = CreateBrush(255, 255, 255)
BrushTexture b, t0, 0, 0
BrushTexture b, t1, 0, 1
BrushTexture b, t2, 0, 2
TextureBlend t1, 2
TextureBlend t2, 3

cube = CreateCube()
PaintMesh cube, b
PositionEntity cube, 0, 0, 0

mode = 0
t = 0
While Not KeyHit(1)
	t = t + 1
	TurnEntity cube, 0.7, 1.1, 0
	
	RotateTexture t1, t * 0.5
	ScaleTexture t1, 1.0 + Sin(t * 0.7) * 0.3, 1.0 + Cos(t * 0.5) * 0.3
	PositionTexture t1, Sin(t * 0.3) * 0.5, Cos(t * 0.4) * 0.5

	If (t Mod 240) = 0 Then mode = (mode + 1) Mod 4
	If mode = 0 Then TextureBlend t2, 3
	If mode = 1 Then TextureBlend t2, 4
	If mode = 2 Then TextureBlend t2, 5
	If mode = 3 Then TextureBlend t2, 2

	UpdateWorld
	RenderWorld
	Text 10, 50, "FPS: " + GetFPS()
	Flip
Wend
End
