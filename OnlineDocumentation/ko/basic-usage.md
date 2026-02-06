---
layout: default
title: 기본 사용 방법
nav_order: 2
parent: 한국어
---

# **기본 사용 방법**

Realtime Destructible Mesh 플러그인을 사용하기 위해서는 Realtime Destructible Mesh 컴포넌트와 Destruction Projectile 컴포넌트를 준비해야 합니다.

블루프린트 액터 또는 레벨에 배치한 액터에 Realtime Destructible Mesh 컴포넌트를 추가합니다.  
![][image1]![][image2]

디테일 패널로 이동해 Source Static Mesh 프로퍼티에 사용할 Static Mesh를 할당합니다.  
![][image3]  
\* 블루프린트 액터로 만들었을 경우 다음 동작부터는 액터를 레벨에 배치한 후 시행해야 합니다.

Realtime Destructible Mesh 컴포넌트의 transform을 조정해줍니다.  
![][image4]

Realtime Destructible Mesh 플러그인은 연산 속도 향상을 위해 원본 메시를 여러 개의 직육면체 청크(chunk)로 쪼개어 관리합니다. **SliceCount** 프로퍼티를 통해 x, y, z축 방향 별 chunk 개수를 설정한 후 **Generate Destructible Chunks** 버튼을 누르면 Chunk 구조가 만들어집니다.  
![][image5]  
\*청크 개수는 메시 크기를 고려해 조절하며, 각 축에 대해 4 이하의 값을 권장합니다.

![][image6]![][image7]  
▲ 직육면체의 source mesh를 2x2x2개의 chunk mesh들로 만든 모습  
  세팅이 완료된 Realtime Destructible Mesh 컴포넌트의 파괴를 트리거하는 역할은 **Destruction Projectile** 컴포넌트가 담당합니다. UDesturctionProjectileComponent::ProcessProjectileHit 메소드의 OtherComp 매개변수에 파괴하고자 하는 Realtime Destructible Mesh 컴포넌트를 인자로 전달하여 파괴를 수행할 수 있습니다.

가장 일반적인 사용방법은 아래와 같이 총탄의 콜리젼 컴포넌트의 자식으로 Destruction Projectile 컴포넌트를 추가하고, On Component Hit 이벤트를 통해 Request Destruction from Projectile 함수를 호출하는 것입니다.  
![][image8]

Hitscan 방식을 채택하여 별도의 투사체가 없는 경우 DestructionProjectile을 플레이어 액터에 추가하고 Request Destruction from Projectile 함수를 호출하여 사용할 수 있습니다.
![][image9]

C++ Raw Api를 직접 사용하고 싶다면 DestructionProjectile의 디테일 패널에서 Auto Bind Hit 체크박스를 활성화 하십시오.
![][image24]
\*현재 투사체에 대한 Raw Api 호출만 지원하고 있습니다. 투사체의 콜리젼 컴포넌트의 자식으로 Destruction Projectile 컴포넌트를 추가하십시오.

[image4]: ../images/image4.png


[image8]: ../images/image8.png


[image1]: ../images/image1.png


[image3]: ../images/image3.png


[image5]: ../images/image5.png


[image2]: ../images/image2.png


[image6]: ../images/image6.png


[image9]: ../images/image9.png


[image7]: ../images/image7.png

[image24]: ../images/image24.png

