/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

import {DataSet} from './scatterPlot';
import {Point2D} from './vector';

/** Shuffles the array in-place in O(n) time using Fisher-Yates algorithm. */
export function shuffle<T>(array: T[]): T[] {
  let m = array.length;
  let t: T;
  let i: number;

  // While there remain elements to shuffle.
  while (m) {
    // Pick a remaining element
    i = Math.floor(Math.random() * m--);
    // And swap it with the current element.
    t = array[m];
    array[m] = array[i];
    array[i] = t;
  }
  return array;
}

/** Retrieves a projected point from the data set as a THREE.js vector */
export function getProjectedPointFromIndex(
    dataSet: DataSet, i: number): THREE.Vector3 {
  return new THREE.Vector3(
      dataSet.points[i].projectedPoint[0], dataSet.points[i].projectedPoint[1],
      dataSet.points[i].projectedPoint[2]);
}

/** Projects a 3d point into screen space */
export function vector3DToScreenCoords(
    cam: THREE.Camera, w: number, h: number, v: THREE.Vector3): Point2D {
  let dpr = window.devicePixelRatio;
  let pv = new THREE.Vector3().copy(v).project(cam);

  // The screen-space origin is at the middle of the screen, with +y up.
  let coords: Point2D =
      [((pv.x + 1) / 2 * w) * dpr, -((pv.y - 1) / 2 * h) * dpr];
  return coords;
}

/**
 * Assert that the condition is satisfied; if not, log user-specified message
 * to the console.
 */
export function assert(condition: boolean, message?: string) {
  if (!condition) {
    message = message || 'Assertion failed';
    throw new Error(message);
  }
}
