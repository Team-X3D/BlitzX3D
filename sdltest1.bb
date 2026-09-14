Graphics3D 800,600,0,2
SetBuffer BackBuffer()
camera=CreateCamera()
PositionEntity camera,0,0,-5
light=CreateLight()
cube=CreateCube()
PositionEntity cube,0,0,0
While Not KeyHit(1)
	TurnEntity cube,1,1,0
	UpdateWorld
	RenderWorld
	Text 10,30,"Mouse: "+MouseX()+","+MouseY()+" Z:"+MouseZ()
	Text 10,20,"FPS: "+GetFPS()
	Flip
Wend
End
