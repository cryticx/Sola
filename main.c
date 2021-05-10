#include "SolaRender.h"

#include <stdio.h>

int main() {
	GLFWwindow* window;
	struct SolaRender renderEngine = {0};
	
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	window = glfwCreateWindow(1280, 720, "Sola", NULL, NULL);
	
	srCreateEngine(&renderEngine, window);
	
	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();
		srRenderFrame(&renderEngine);
	}
	srDestroyEngine(&renderEngine);
	
	glfwDestroyWindow(window);
	glfwTerminate();
	
	return 0;
}
