NAME := App_a1

$(NAME)_SOURCES := main.c

$(NAME)_COMPONENTS := daemons/http_server protocols/mqtt protocols/SNTP daemons/ota_server
