#include "EnginePCH.h"
#include "ObjectFactory.h"
#include "Class.h"

UObject* FObjectFactory::ConstructObject(UClass* Class, UObject* Outer, FName Name)
{
    if (!Class || !Class->Constructor)
        return nullptr;
    
    UObject* Object = Class->Constructor();
    Object->ClassPrivate = Class;
    HashObject(Object, Class);

    Object->SetOuter(Outer);

    Name = MakeUniqueObjectName( Class, Outer, Name);

    Object->SetName(Name);

    LOG(Info, "Create {}", Class->Name);
    //LOG(Info, "Total Allocation Bytes - {}", FEngineStatics::TotalAllocationBytes);
    //LOG(Info, "Total Allocation Count - {}", FEngineStatics::TotalAllocationCount);

    return Object;
}

FName FObjectFactory::MakeUniqueObjectName(const UClass* Class, UObject* Outer, FName BaseName)
{
    if (!Class)
        return FName();

    // 이름을 따로 안 줬으면 Class 이름을 기본 이름으로 사용
    if (BaseName == NAME_None)
    {
        BaseName = FName(Class->Name);
    }

    FString BaseNameString = BaseName.ToString();

    int32 Number = 0;

    while (true)
    {
        FString CandidateName =  BaseNameString + "_" + std::to_string(Number);

        bool bNameExists = false;

        for (UObject* Object : GUObjectArray)
        {
            if (!Object)
                continue;

            if (Object->GetOuter() != Outer)
                continue;

            if (Object->GetName() == CandidateName)
            {
                bNameExists = true;
                break;
            }
        }

        if (!bNameExists)
        {
            return FName(CandidateName);
        }

        ++Number;
    }
}
