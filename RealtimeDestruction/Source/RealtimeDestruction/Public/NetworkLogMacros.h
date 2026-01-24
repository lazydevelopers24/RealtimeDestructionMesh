// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// NetworkLogMacros.h
// 네트워크 디버깅용 로그 매크로 시스템

#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

// 로그 카테고리 선언
DECLARE_LOG_CATEGORY_EXTERN(LogNetwork, Log, All);

//=============================================================================
// 네트워크 역할 문자열 반환 함수들
//=============================================================================

// 네트워크 역할을 문자열로 변환
inline FString GetNetRoleString(ENetRole Role)
{
    switch (Role)
    {
    case ROLE_None:             return TEXT("None");
    case ROLE_SimulatedProxy:   return TEXT("SimProxy");
    case ROLE_AutonomousProxy:  return TEXT("AutoProxy");
    case ROLE_Authority:        return TEXT("Authority");
    default:                    return TEXT("Unknown");
    }
}

// 월드에서 네트워크 모드 문자열 반환
inline FString GetNetModeString(UWorld* World)
{
    if (!World) return TEXT("[NoWorld]");

    switch (World->GetNetMode())
    {
    case NM_Standalone:         return TEXT("[Standalone]");
    case NM_DedicatedServer:    return TEXT("[Server]");
    case NM_ListenServer:       return TEXT("[ListenServer]");
    case NM_Client:             return TEXT("[Client]");
    default:                    return TEXT("[Unknown]");
    }
}

// 오브젝트에서 네트워크 모드 문자열 반환
inline FString GetNetModeString(const UObject* Object)
{
    if (!Object) return TEXT("[NoObject]");
    return GetNetModeString(Object->GetWorld());
}

//=============================================================================
// 액터 정보 추출 함수들
//=============================================================================

// 액터의 전체 네트워크 정보 문자열 반환
inline FString GetActorNetInfo(AActor* Actor)
{
    if (!Actor)
    {
        return TEXT("Actor=NULL");
    }

    FString OwnerName = Actor->GetOwner() ? Actor->GetOwner()->GetName() : TEXT("None");
    FString InstigatorName = Actor->GetInstigator() ? Actor->GetInstigator()->GetName() : TEXT("None");
    UNetConnection* NetConn = Actor->GetNetConnection();

    return FString::Printf(TEXT("%s | Local:%s Remote:%s | Owner:%s | NetConn:%s"),
        *Actor->GetName(),
        *GetNetRoleString(Actor->GetLocalRole()),
        *GetNetRoleString(Actor->GetRemoteRole()),
        *OwnerName,
        NetConn ? TEXT("Valid") : TEXT("NULL"));
}

// 컴포넌트의 Owner 액터에서 네트워크 정보 추출
inline FString GetComponentNetInfo(UActorComponent* Component)
{
    if (!Component)
    {
        return TEXT("Component=NULL");
    }

    AActor* Owner = Component->GetOwner();
    if (!Owner)
    {
        return FString::Printf(TEXT("%s | OwnerActor=NULL"), *Component->GetName());
    }

    return FString::Printf(TEXT("%s -> %s"), *Component->GetName(), *GetActorNetInfo(Owner));
}

//=============================================================================
// 기본 네트워크 로그 매크로
//=============================================================================

// 기본 네트워크 로그 (네트워크 모드 자동 표시)
#define NET_LOG(Object, Format, ...) \
    UE_LOG(LogNetwork, Log, TEXT("%s %s"), \
        *GetNetModeString(Object), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

#define NET_LOG_WARNING(Object, Format, ...) \
    UE_LOG(LogNetwork, Warning, TEXT("%s %s"), \
        *GetNetModeString(Object), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

#define NET_LOG_ERROR(Object, Format, ...) \
    UE_LOG(LogNetwork, Error, TEXT("%s %s"), \
        *GetNetModeString(Object), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

//=============================================================================
// 액터용 네트워크 로그 매크로
//=============================================================================

// 액터의 네트워크 정보와 함께 로그 출력
#define NET_LOG_ACTOR(Actor, Format, ...) \
    UE_LOG(LogNetwork, Log, TEXT("%s [%s] %s"), \
        *GetNetModeString(Actor), \
        *GetActorNetInfo(Actor), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

#define NET_LOG_ACTOR_WARNING(Actor, Format, ...) \
    UE_LOG(LogNetwork, Warning, TEXT("%s [%s] %s"), \
        *GetNetModeString(Actor), \
        *GetActorNetInfo(Actor), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

#define NET_LOG_ACTOR_ERROR(Actor, Format, ...) \
    UE_LOG(LogNetwork, Error, TEXT("%s [%s] %s"), \
        *GetNetModeString(Actor), \
        *GetActorNetInfo(Actor), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

//=============================================================================
// 컴포넌트용 네트워크 로그 매크로
//=============================================================================

// 컴포넌트의 네트워크 정보와 함께 로그 출력
#define NET_LOG_COMPONENT(Component, Format, ...) \
    UE_LOG(LogNetwork, Log, TEXT("%s [%s] %s"), \
        *GetNetModeString(Component), \
        *GetComponentNetInfo(Component), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

#define NET_LOG_COMPONENT_WARNING(Component, Format, ...) \
    UE_LOG(LogNetwork, Warning, TEXT("%s [%s] %s"), \
        *GetNetModeString(Component), \
        *GetComponentNetInfo(Component), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

#define NET_LOG_COMPONENT_ERROR(Component, Format, ...) \
    UE_LOG(LogNetwork, Error, TEXT("%s [%s] %s"), \
        *GetNetModeString(Component), \
        *GetComponentNetInfo(Component), \
        *FString::Printf(TEXT(Format), ##__VA_ARGS__))

//=============================================================================
// RPC 디버깅 매크로
//=============================================================================

// Server RPC 호출 전 검증 및 로그
#define NET_LOG_SERVER_RPC(Actor, RPCName) \
    { \
        UNetConnection* __NetConn = Actor ? Actor->GetNetConnection() : nullptr; \
        if (__NetConn) \
        { \
            UE_LOG(LogNetwork, Log, TEXT("%s [%s] Server RPC '%s' 호출 - NetConnection 유효"), \
                *GetNetModeString(Actor), *GetActorNetInfo(Actor), TEXT(#RPCName)); \
        } \
        else \
        { \
            UE_LOG(LogNetwork, Error, TEXT("%s [%s] Server RPC '%s' 실패 예상 - NetConnection NULL!"), \
                *GetNetModeString(Actor), *GetActorNetInfo(Actor), TEXT(#RPCName)); \
        } \
    }

// Client RPC 로그
#define NET_LOG_CLIENT_RPC(Actor, RPCName) \
    UE_LOG(LogNetwork, Log, TEXT("%s [%s] Client RPC '%s' 호출"), \
        *GetNetModeString(Actor), *GetActorNetInfo(Actor), TEXT(#RPCName))

// Multicast RPC 로그
#define NET_LOG_MULTICAST_RPC(Actor, RPCName) \
    UE_LOG(LogNetwork, Log, TEXT("%s [%s] Multicast RPC '%s' 호출"), \
        *GetNetModeString(Actor), *GetActorNetInfo(Actor), TEXT(#RPCName))

//=============================================================================
// 소유권 디버깅 매크로
//=============================================================================

// 액터의 전체 소유권 체인 출력
#define NET_LOG_OWNERSHIP(Actor) \
    { \
        if (Actor) \
        { \
            AActor* __Current = Actor; \
            FString __Chain = __Current->GetName(); \
            while (__Current->GetOwner()) \
            { \
                __Current = __Current->GetOwner(); \
                __Chain += TEXT(" -> ") + __Current->GetName(); \
            } \
            UE_LOG(LogNetwork, Warning, TEXT("%s === 소유권 체인 === %s"), \
                *GetNetModeString(Actor), *__Chain); \
            UE_LOG(LogNetwork, Warning, TEXT("%s   HasAuthority: %s"), \
                *GetNetModeString(Actor), Actor->HasAuthority() ? TEXT("true") : TEXT("false")); \
            UE_LOG(LogNetwork, Warning, TEXT("%s   LocalRole: %s, RemoteRole: %s"), \
                *GetNetModeString(Actor), \
                *GetNetRoleString(Actor->GetLocalRole()), \
                *GetNetRoleString(Actor->GetRemoteRole())); \
            UE_LOG(LogNetwork, Warning, TEXT("%s   NetConnection: %s"), \
                *GetNetModeString(Actor), \
                Actor->GetNetConnection() ? TEXT("Valid") : TEXT("NULL")); \
            UE_LOG(LogNetwork, Warning, TEXT("%s   Instigator: %s"), \
                *GetNetModeString(Actor), \
                Actor->GetInstigator() ? *Actor->GetInstigator()->GetName() : TEXT("None")); \
        } \
        else \
        { \
            UE_LOG(LogNetwork, Error, TEXT("[Unknown] === 소유권 체인 === Actor is NULL!")); \
        } \
    }

// 컴포넌트용 소유권 디버깅 (Owner 액터 기준)
#define NET_LOG_COMPONENT_OWNERSHIP(Component) \
    { \
        if (Component) \
        { \
            AActor* __Owner = Component->GetOwner(); \
            UE_LOG(LogNetwork, Warning, TEXT("%s === 컴포넌트 소유권 === %s"), \
                *GetNetModeString(Component), *Component->GetName()); \
            NET_LOG_OWNERSHIP(__Owner); \
        } \
        else \
        { \
            UE_LOG(LogNetwork, Error, TEXT("[Unknown] === 컴포넌트 소유권 === Component is NULL!")); \
        } \
    }

//=============================================================================
// 조건부 네트워크 로그 매크로 (서버/클라이언트에서만 실행)
//=============================================================================

// 서버에서만 로그 출력
#define NET_LOG_SERVER_ONLY(Object, Format, ...) \
    { \
        UWorld* __World = Object ? Object->GetWorld() : nullptr; \
        if (__World && (__World->GetNetMode() == NM_DedicatedServer || __World->GetNetMode() == NM_ListenServer)) \
        { \
            NET_LOG(Object, Format, ##__VA_ARGS__); \
        } \
    }

// 클라이언트에서만 로그 출력
#define NET_LOG_CLIENT_ONLY(Object, Format, ...) \
    { \
        UWorld* __World = Object ? Object->GetWorld() : nullptr; \
        if (__World && __World->GetNetMode() == NM_Client) \
        { \
            NET_LOG(Object, Format, ##__VA_ARGS__); \
        } \
    }

//=============================================================================
// 화면 디버그 출력 매크로 (GEngine->AddOnScreenDebugMessage)
//=============================================================================

// 화면에 네트워크 정보 출력 (색상 자동 구분: 서버=녹색, 클라이언트=파랑)
#define NET_SCREEN_LOG(Object, Duration, Format, ...) \
    { \
        if (GEngine) \
        { \
            UWorld* __World = Object ? Object->GetWorld() : nullptr; \
            FColor __Color = FColor::White; \
            FString __Prefix = TEXT("[Unknown]"); \
            if (__World) \
            { \
                switch (__World->GetNetMode()) \
                { \
                case NM_DedicatedServer: __Color = FColor::Green; __Prefix = TEXT("[Server]"); break; \
                case NM_ListenServer: __Color = FColor::Yellow; __Prefix = TEXT("[ListenServer]"); break; \
                case NM_Client: __Color = FColor::Cyan; __Prefix = TEXT("[Client]"); break; \
                case NM_Standalone: __Color = FColor::White; __Prefix = TEXT("[Standalone]"); break; \
                default: break; \
                } \
            } \
            GEngine->AddOnScreenDebugMessage(-1, Duration, __Color, \
                FString::Printf(TEXT("%s %s"), *__Prefix, *FString::Printf(TEXT(Format), ##__VA_ARGS__))); \
        } \
    }

// 고유 키를 사용한 화면 출력 (같은 키는 업데이트됨)
#define NET_SCREEN_LOG_KEY(Object, Key, Duration, Format, ...) \
    { \
        if (GEngine) \
        { \
            UWorld* __World = Object ? Object->GetWorld() : nullptr; \
            FColor __Color = FColor::White; \
            FString __Prefix = TEXT("[Unknown]"); \
            if (__World) \
            { \
                switch (__World->GetNetMode()) \
                { \
                case NM_DedicatedServer: __Color = FColor::Green; __Prefix = TEXT("[Server]"); break; \
                case NM_ListenServer: __Color = FColor::Yellow; __Prefix = TEXT("[ListenServer]"); break; \
                case NM_Client: __Color = FColor::Cyan; __Prefix = TEXT("[Client]"); break; \
                case NM_Standalone: __Color = FColor::White; __Prefix = TEXT("[Standalone]"); break; \
                default: break; \
                } \
            } \
            GEngine->AddOnScreenDebugMessage(Key, Duration, __Color, \
                FString::Printf(TEXT("%s %s"), *__Prefix, *FString::Printf(TEXT(Format), ##__VA_ARGS__))); \
        } \
    }
