---
layout: default
title: 파괴 형상 및 데칼 커스텀
nav_order: 3
parent: 한국어
---

# **파괴 형상 및 데칼 커스텀**

# ![][image10]

  Realtime Destructible Mesh 플러그인은 파괴 형상 커스텀과 시각적 충실도(visual fidelity) 향상을 위해 Impact Profile 시스템을 지원합니다. 이 기능을 사용하기 위해서는 아래와 같은 과정이 필요합니다.

1. Impact Profile Data Asset 인스턴스 생성  
2. 파괴 형상과 데칼 데이터를 Impact Profile Editor를 통해 편집  
3. 프로젝트 세팅 \- RDM Settings에 데이터 애셋 인스턴스 등록  
4. Projectile과 Mesh에 각각 Config ID와 Surface Type 설정

콘텐츠 드로어에서 Add → Miscellaneous → DataAsset을 선택하고 **Impact Profile Data Asset** 인스턴스를 생성합니다.  
![][image11]  
인스턴스를 만들면 가장 먼저 Config ID를 설정합니다. 각 Destruction Projectile 컴포넌트는 자신이 어떤 Impact Profile Data Asset을 사용해 파괴 효과를 낼지를 Config ID를 통해 식별합니다.

Surface Configs는 메시에 정의된 Surface Type에 따라 사용할 파괴 형상과 데칼을 정의합니다. **Open Impact Profile Editor** 버튼을 누르면 프리뷰를 제공하는 Impact Profile Editor를 통해  Surface Configs를 상세하게 설정할 수 있습니다.  
![][image12]

임팩트 프로필 에디터에서 편집할 수 있는 내용들은 다음과 같습니다.

- Tool shape(projectile에 의해 파괴되는 형태)의 종류와 transform  
- 착탄 지점에 생성될 decal에 적용할 material과 decal의 transform

![][image13]

설정이 완료되면 Project Setting를 열고 Impact Profile Settings \- Impact Profiles 배열에 새로 만든 Decal Material Data Asset를 추가합니다.  
![][image14]

마지막으로 Destruction Projectile 컴포넌트의 **Decal Config ID**와 Realtime Destructible Mesh의 **Surface Type**을 설정합니다. 이후 투사체와 메시가 충돌했을 때  Data Assets에서 알맞은 Config ID와 Surface Type 쌍을 찾아 설정대로 메쉬 파괴 및 데칼 스폰이 이루어집니다.  
![][image15]  
▲Destruction Projectile 컴포넌트의 Decal 카테고리에서 Decal Config ID를 찾을 수 있습니다.  
![][image16]  
▲Realtime Destructible Mesh컴포넌트의 Hole Decal 카테고리에서 Surface Type을 찾을 수 있습니다.

[image13]: ../images/image13.png


[image11]: ../images/image11.png


[image12]: ../images/image12.png


[image15]: ../images/image15.png


[image10]: ../images/image10.png


[image16]: ../images/image16.png


[image14]: ../images/image14.png

