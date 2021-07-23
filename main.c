#include "SolaRender.h"

#include <math.h>
#include <stdio.h>

#define likely(x)	__builtin_expect((x), 1)

int main() {
	struct SolaRender renderEngine;
	struct CursorPosition {
		double x;
		double y;
		double previousX;
		double previousY;
	} cursorPos;
	
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	
	srCreateEngine(&renderEngine, glfwCreateWindow(1280, 720, "Sola", NULL, NULL));

	glfwSetInputMode(renderEngine.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	
	if (glfwRawMouseMotionSupported())
		glfwSetInputMode(renderEngine.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	
	glfwGetCursorPos(renderEngine.window, &cursorPos.x, &cursorPos.y);
	
	vec3 cameraPos = {0};
	vec2 cameraOrientation = {0};
	
	while (likely(!glfwWindowShouldClose(renderEngine.window))) { //TODO optimize inverse-view/camera matrix
		glfwPollEvents();
		
		cursorPos.previousX = cursorPos.x, cursorPos.previousY = cursorPos.y;
		glfwGetCursorPos(renderEngine.window, &cursorPos.x, &cursorPos.y);
		
		cameraOrientation[0] += (float) fmod((cursorPos.previousX - cursorPos.x) * 0.001f, GLM_PI * 2);
		cameraOrientation[1] += (float) fmod((cursorPos.previousY - cursorPos.y) * 0.001f, CGLM_PI_2);
		
		float cosCam = cosf(-cameraOrientation[0]) * 0.1f, sinCam = sinf(-cameraOrientation[0]) * 0.1f; // input should translate relative to orientation
		
		if (glfwGetKey(renderEngine.window, GLFW_KEY_W)) {
			cameraPos[0] += sinCam;
			cameraPos[2] -= cosCam;
		}
		if (glfwGetKey(renderEngine.window, GLFW_KEY_A)) {
			cameraPos[0] -= cosCam;
			cameraPos[2] -= sinCam;
		}
		if (glfwGetKey(renderEngine.window, GLFW_KEY_S)) {
			cameraPos[0] -= sinCam;
			cameraPos[2] += cosCam;
		}
		if (glfwGetKey(renderEngine.window, GLFW_KEY_D)) {
			cameraPos[0] += cosCam;
			cameraPos[2] += sinCam;
		}
		cameraPos[1] += (glfwGetKey(renderEngine.window, GLFW_KEY_SPACE) - glfwGetKey(renderEngine.window, GLFW_KEY_C)) * 0.1f;
		
		glm_mat4_identity(renderEngine.uniformData.viewInverse);
		
		glm_translate(renderEngine.uniformData.viewInverse, cameraPos);
		
		glm_rotate(renderEngine.uniformData.viewInverse, cameraOrientation[0], (vec3) { 0.f, 1.f, 0.f });
		glm_rotate(renderEngine.uniformData.viewInverse, cameraOrientation[1], (vec3) { 1.f, 0.f, 0.f });
		
		srRenderFrame(&renderEngine);
	}
	srDestroyEngine(&renderEngine);
	
	glfwDestroyWindow(renderEngine.window);
	glfwTerminate();
	
	return 0;
}
