#ifndef CUSTOMCONTAINERTOPBAR_HPP
#define CUSTOMCONTAINERTOPBAR_HPP

#include <gui_generated/containers/CustomContainerTopBarBase.hpp>

class CustomContainerTopBar : public CustomContainerTopBarBase
{
public:
    CustomContainerTopBar();
    virtual ~CustomContainerTopBar() {}

    virtual void initialize();
protected:
};

#endif // CUSTOMCONTAINERTOPBAR_HPP
