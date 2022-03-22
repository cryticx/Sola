#include "SolaRender.h"

#include <math.h>
#include <stdio.h>

#define likely(x)	__builtin_expect((x), 1)

int main() {
	SolaRender renderEngine;
	
	struct CursorPosition {
		double x;
		double y;
		double prevX;
		double prevY;
	} cursorPos;
	
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	
	srCreateEngine(&renderEngine, glfwCreateWindow(1920, 1080, "Sola", NULL, NULL));

	glfwSetInputMode(renderEngine.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	
	if (glfwRawMouseMotionSupported())
		glfwSetInputMode(renderEngine.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	
	glfwGetCursorPos(renderEngine.window, &cursorPos.x, &cursorPos.y);
	
	vec3 cameraPos			= {0};
	vec2 cameraOrientation	= {0};

	double currTime = glfwGetTime();

	while (likely(!glfwWindowShouldClose(renderEngine.window))) {
		double prevTime		= currTime;

		currTime			= glfwGetTime();

		double deltaTime	= currTime - prevTime;

		float moveDelta		= deltaTime * 5.f;

		glfwPollEvents();
		
		cursorPos.prevX = cursorPos.x, cursorPos.prevY = cursorPos.y;
		glfwGetCursorPos(renderEngine.window, &cursorPos.x, &cursorPos.y);
		
		cameraOrientation[0] += (float) fmod((cursorPos.prevX - cursorPos.x) * 0.001f, GLM_PI * 2);
		cameraOrientation[1] += (float) fmod((cursorPos.prevY - cursorPos.y) * 0.001f, CGLM_PI_2);

		// translate relative to orientation
		float cosYaw = cosf(-cameraOrientation[0]), sinYaw = sinf(-cameraOrientation[0]), cosPitch = sinf(-cameraOrientation[1]);

		if (glfwGetKey(renderEngine.window, GLFW_KEY_W)) {
			cameraPos[0] += moveDelta * sinYaw;
			cameraPos[1] -= moveDelta * cosPitch;
			cameraPos[2] -= moveDelta * cosYaw;
		}
		if (glfwGetKey(renderEngine.window, GLFW_KEY_A)) {
			cameraPos[0] -= moveDelta * cosYaw;
			cameraPos[2] -= moveDelta * sinYaw;
		}
		if (glfwGetKey(renderEngine.window, GLFW_KEY_S)) {
			cameraPos[0] -= moveDelta * sinYaw;
			cameraPos[1] += moveDelta * cosPitch;
			cameraPos[2] += moveDelta * cosYaw;
		}
		if (glfwGetKey(renderEngine.window, GLFW_KEY_D)) {
			cameraPos[0] += moveDelta * cosYaw;
			cameraPos[2] += moveDelta * sinYaw;
		}
		cameraPos[1] += moveDelta * (glfwGetKey(renderEngine.window, GLFW_KEY_SPACE) - glfwGetKey(renderEngine.window, GLFW_KEY_C));

		glm_mat4_identity(renderEngine.rayGenUniform.viewInverse);
		
		glm_translate(renderEngine.rayGenUniform.viewInverse, cameraPos);
		
		glm_rotate(renderEngine.rayGenUniform.viewInverse, cameraOrientation[0], (vec3) { 0.f, 1.f, 0.f });
		glm_rotate(renderEngine.rayGenUniform.viewInverse, cameraOrientation[1], (vec3) { 1.f, 0.f, 0.f });
		
		srRenderFrame(&renderEngine);
	}
	srDestroyEngine(&renderEngine);
	
	glfwDestroyWindow(renderEngine.window);
	glfwTerminate();
	
	return 0;
}
