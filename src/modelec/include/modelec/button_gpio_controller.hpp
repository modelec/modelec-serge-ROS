#pragma once

#include <rclcpp/rclcpp.hpp>
#include <modelec_interface/srv/new_button.hpp>
#include <modelec_interface/srv/button.hpp>
#include <modelec_interface/msg/button.hpp>
#include <wiringPi.h>

namespace Modelec {
    class ButtonGpioController : public rclcpp::Node {

        struct Button {
            int pin;
            rclcpp::Publisher<modelec_interface::msg::Button>::SharedPtr publisher;
            std::string name;
        };

    public:
        ButtonGpioController();

    private:
        // service
        rclcpp::Service<modelec_interface::srv::NewButton>::SharedPtr new_button_service_;
        rclcpp::Service<modelec_interface::srv::Button>::SharedPtr button_server_;

        // service callbacks
        void new_button(const std::shared_ptr<modelec_interface::srv::NewButton::Request> request, std::shared_ptr<modelec_interface::srv::NewButton::Response> response);
        void check_button(const std::shared_ptr<modelec_interface::srv::Button::Request> request, std::shared_ptr<modelec_interface::srv::Button::Response> response);

        // timer
        rclcpp::TimerBase::SharedPtr timer_;

        std::map<int, Button> buttons_;
    };
}