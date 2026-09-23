#include "EnginePCH.h"

// Source/Editor/main.cpp  — exe. 여기서만 구체 타입을 안다.
#include "Core/EntryPoint.h"

#include "Editor/HitoriEd/Engine.h"
#include "Programs/ObjViewer/ObjViewerApp.h"

TUniquePtr<FApplication> CreateApplication()
{
#ifdef OBJ_VIEWER
	return MakeUnique<FObjViewerApp>();
#else
	return MakeUnique<FEditorApplication>();
#endif
}