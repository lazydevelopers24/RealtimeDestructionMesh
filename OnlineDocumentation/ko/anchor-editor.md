---
layout: default
title: 앵커 에디터
nav_order: 5
parent: 한국어
---

# **심화기능: 앵커 에디터**

메시에 파괴가 누적되다보면 하부에 지지받지 못하고 공중에 뜬 조각이 생길 수 있습니다. 이를 mesh island라고 합니다. Realtime Destructible Mesh 플러그인에는 mesh island를 감지하고 별도의 조각으로 스폰해주거나 삭제해주는 구조적 무결성(structural integrity) 시스템이 탑재되어 있습니다.

메시는 mesh island를 탐지하기 위해 내부적으로 수많은 직육면체 셀(cell)로 근사되며, 그 중 일부가 메시 구조를 지지하는 앵커(anchor)가 됩니다. 구조적 무결성 시스템은 기본적으로 메시의 최하단(-z 방향) 몇 개의 층을 이루는 셀들을 앵커로 가정합니다. (설정값에 따라 달라집니다.)  
![][image19]![][image20]  
▲ Realtime Destructible Mesh의 셀들을 표시한 모습. 연두색이 앵커 셀입니다.

그러나 벽간판처럼 앵커를 옆이나 위로 설정해야 하는 일이 생길 수 있습니다. 이러한 경우 Anchor Editor를 이용하면 자유롭게 앵커 셀들을 지정해줄 수 있습니다.  
Editor mode selector에서 Anchor Editor를 선택합니다.  
![][image21]  
Anchor Editor를 선택하면 Anchor Editor 패널이 나타납니다.  
Spawn Anchor Plane 또는 Spawn Anchor Volume 버튼을 누르면 앵커 영역을 지정하는 평면 또는 박스 볼륨이 생성됩니다. 앵커 평면 또는 앵커 볼륨이 Realtime Destructible Mesh와 겹치도록 조정하고 ApplyAllAnchorPlanes /  ApplyAllAnchorVolumes / ApplyAnchors 버튼을 누르면 앵커 도형과 메시의 겹치는 영역에 들어간 셀들이 앵커로 지정됩니다.

아래 사진은 Spawn Anchor Volume 후 앵커 박스를 메시 측면과 겹치도록 옮기고 Apply All Anchor Volumes를 적용한 결과입니다. 2945개의 셀이 앵커로 지정되었습니다.  
![][image22]

![][image23]
▲ Grid Cell 디버그를 켜서 앵커가 제대로 설정된 것을 확인할 수 있습니다.



[image22]: ../images/image22.png
[image19]: ../images/image19.png
[image20]: ../images/image20.png
[image21]: ../images/image21.png
[image23]: ../images/image23.png